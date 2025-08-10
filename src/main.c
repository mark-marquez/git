#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/sha.h>
#include <dirent.h>
#include <time.h>


typedef struct {
    char mode[7];               // e.g. "100644" is a file
    char *file_name;            // allocated dynamically
    unsigned char raw_hash[20];
} Entry; 


typedef struct {
    Entry *entries;
    size_t count;
} Tree;

unsigned char *hash_blob_object(char *file_name, char* flag);
void hash_to_hex(char* hex_buf, const unsigned char *raw_hash);


// Sort tree entries by filename (required for Git-canonical tree hash)
static int cmp_entry_by_name(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a, *eb = (const Entry *)b;
    return strcmp(ea->file_name, eb->file_name);
}

void create_tree_object(const char *dirpath, Tree *tree, unsigned char tree_hash[20]) {
    DIR *dir = opendir(dirpath);
    if (!dir) { perror("opendir"); return; }

    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0 || strcmp(dent->d_name, ".git") == 0)
            continue;

        char subpath[PATH_MAX];
        snprintf(subpath, sizeof(subpath), "%s/%s", dirpath, dent->d_name);

        char *file_name = malloc(strlen(dent->d_name) + 1);
        strcpy(file_name, dent->d_name);

        // Decide mode and raw_hash based on entry type
        char mode[7];
        unsigned char raw_hash[20];

        if (dent->d_type == DT_REG) { // regular file
            strcpy(mode, "100644");
            unsigned char *hash = hash_blob_object(subpath, ""); // assuming this both hashes and writes as needed
            memcpy(raw_hash, hash, 20);
            free(hash);
        } else if (dent->d_type == DT_DIR) { // directory
            strcpy(mode, "40000");
            Tree new_tree = (Tree){ NULL, 0 };
            unsigned char new_tree_hash[20];
            create_tree_object(subpath, &new_tree, new_tree_hash);
            memcpy(raw_hash, new_tree_hash, 20);
        } else if (dent->d_type == DT_UNKNOWN) {
            // Some filesystems don't fill d_type; fall back to lstat
            struct stat st;
            if (lstat(subpath, &st) == -1) { perror("lstat"); free(file_name); continue; }
            if (S_ISREG(st.st_mode)) {
                strcpy(mode, "100644");
                unsigned char *hash = hash_blob_object(subpath, "");
                memcpy(raw_hash, hash, 20);
                free(hash);
            } else if (S_ISDIR(st.st_mode)) {
                strcpy(mode, "40000");
                Tree new_tree = (Tree){ NULL, 0 };
                unsigned char new_tree_hash[20];
                create_tree_object(subpath, &new_tree, new_tree_hash);
                memcpy(raw_hash, new_tree_hash, 20);
            } else {
                free(file_name);
                continue; // skip other types (symlinks, devices, etc.)
            }
        } else {
            free(file_name);
            continue; // skip non-regular/non-directory types
        }

        // Store the Entry
        tree->entries = realloc(tree->entries, sizeof(Entry) * (tree->count + 1));
        Entry *entry = &tree->entries[tree->count++];
        strcpy(entry->mode, mode);
        entry->file_name = file_name;
        memcpy(entry->raw_hash, raw_hash, 20);
    }

    closedir(dir);

    // Canonicalize order: sort entries by filename before hashing/writing
    if (tree->count > 1) {
        qsort(tree->entries, tree->count, sizeof(Entry), cmp_entry_by_name);
    }

    // Compute tree payload size
    size_t tree_size = 0;
    for (int i = 0; i < tree->count; i++) {
        Entry *e = &tree->entries[i];
        tree_size += strlen(e->mode) + 1;    // "<mode><space>"
        tree_size += strlen(e->file_name) + 1; // "<name><NUL>"
        tree_size += 20;                       // 20-byte raw SHA
    }

    // "tree <size>\0" header + payload
    char header[64];
    int header_len = sprintf(header, "tree %zu", tree_size) + 1; // +1 for '\0'
    size_t buf_size = header_len + tree_size;

    unsigned char *tree_data = malloc(buf_size);
    memcpy(tree_data, header, header_len);

    // Serialize entries
    unsigned char *p = tree_data + header_len;
    for (int i = 0; i < tree->count; i++) {
        Entry *e = &tree->entries[i];
        p += sprintf((char *)p, "%s %s", e->mode, e->file_name) + 1; // includes the trailing NUL
        memcpy(p, e->raw_hash, 20);
        p += 20;
    }

    // Hash the uncompressed tree object
    unsigned char sha[20];
    SHA1(tree_data, buf_size, sha);
    memcpy(tree_hash, sha, 20);

    // Compress to zlib format (use compressBound to avoid overflow)
    uLongf cap = compressBound(buf_size);
    unsigned char *zbuf = malloc(cap);
    z_stream s = (z_stream){0};
    if (deflateInit(&s, Z_DEFAULT_COMPRESSION) != Z_OK) {
        fprintf(stderr, "deflateInit failed\n");
        free(tree_data);
        free(zbuf);
        return;
    }

    s.next_in  = tree_data; s.avail_in  = (uInt)buf_size;
    s.next_out = zbuf;      s.avail_out = cap;

    int st = deflate(&s, Z_FINISH);
    if (st != Z_STREAM_END) {
        fprintf(stderr, "deflate error: %d (in=%zu, out cap=%lu, out=%lu)\n",
                st, (size_t)buf_size, (unsigned long)cap, (unsigned long)s.total_out);
        deflateEnd(&s);
        free(tree_data);
        free(zbuf);
        return;
    }
    size_t zlen = s.total_out;
    deflateEnd(&s);

    // Write to .git/objects/xx/yyyy...
    char hex[41];
    hash_to_hex(hex, sha);

    char directory[64], path[128];
    snprintf(directory,  sizeof(directory), ".git/objects/%.2s", hex);
    mkdir(directory, 0755);
    snprintf(path, sizeof(path), ".git/objects/%.2s/%.38s", hex, hex + 2);

    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); free(tree_data); free(zbuf); return; }
    fwrite(zbuf, 1, zlen, fp);
    fclose(fp);

    free(tree_data);
    free(zbuf);
}


int decompress_data(unsigned char *buffer, const unsigned char *compressed_data, size_t compressed_size) {
    z_stream stream = {0};
    stream.next_in = (unsigned char *)compressed_data;
    stream.avail_in = compressed_size;

    stream.next_out = buffer;
    stream.avail_out = 16384;

    if (inflateInit(&stream) != Z_OK) return -1;

    int status = inflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        fprintf(stderr, "inflate error: %d\n", status);
        inflateEnd(&stream);
        return -1;
    }

    inflateEnd(&stream);
    return stream.total_out;  // return decompressed length
}


long get_file_size(FILE *fp) {
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);
    return file_size;
} 



void build_path(char* full_path, size_t buf_size, const char *object_hash) {
    snprintf(full_path, buf_size, ".git/objects/%.2s/%.38s", object_hash, object_hash + 2);
}


void hash_to_hex(char* hex_buf, const unsigned char *raw_hash) {
    for (int i = 0; i < 20; i++) {
        sprintf(hex_buf + (i * 2), "%02x", raw_hash[i]);
    }
    hex_buf[40] = '\0';
}


unsigned char *hash_blob_object(char *file_name, char* flag) {
    FILE *fp = NULL, *new_fp = NULL;
    unsigned char *blob = NULL, *compressed = NULL, *hash = NULL;
    size_t total_size = 0;
    uLongf comp_cap = 0;
    size_t zlen = 0;

    // 1) Open input
    fp = fopen(file_name, "rb");
    if (!fp) { perror("fopen"); goto cleanup; }

    long file_length = get_file_size(fp);
    if (file_length < 0) { fprintf(stderr, "get_file_size failed\n"); goto cleanup; }

    // 2) Build "blob <len>\0" header + content
    char header[64];
    size_t header_length = (size_t)snprintf(header, sizeof(header), "blob %ld", file_length);
    if (header_length + 1 > sizeof(header)) { fprintf(stderr, "header overflow\n"); goto cleanup; }

    total_size = header_length + 1 + (size_t)file_length;
    blob = (unsigned char *)malloc(total_size);
    if (!blob) { perror("malloc"); goto cleanup; }

    memcpy(blob, header, header_length);
    blob[header_length] = '\0';

    if (fread(blob + header_length + 1, 1, (size_t)file_length, fp) != (size_t)file_length) {
        perror("fread");
        goto cleanup;
    }

    // 3) SHA1 of uncompressed object
    unsigned char raw_hash[20];
    SHA1(blob, total_size, raw_hash);
    char hex_hash[41];
    hash_to_hex(hex_hash, raw_hash);

    // 4) Compress (use compressBound to avoid overflow)
    z_stream stream = (z_stream){0};
    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        fprintf(stderr, "deflateInit failed\n");
        goto cleanup;
    }

    comp_cap = compressBound(total_size);
    compressed = (unsigned char *)malloc(comp_cap);
    if (!compressed) { perror("malloc"); deflateEnd(&stream); goto cleanup; }

    stream.next_in  = blob;
    stream.avail_in = (uInt)total_size;
    stream.next_out = compressed;
    stream.avail_out = comp_cap;

    int status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        fprintf(stderr, "Compression overflow.\n");
        deflateEnd(&stream);
        goto cleanup;  // don't touch/free 'compressed' again below if we bail here
    }
    zlen = (size_t)stream.total_out;
    deflateEnd(&stream);

    // 5) Write .git/objects/xx/yyyy...
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), ".git/objects/%.2s", hex_hash);
    // mkdir may already exist; that's fine
    (void)mkdir(dir_path, 0755);

    char write_path[256];
    snprintf(write_path, sizeof(write_path), ".git/objects/%.2s/%.38s", hex_hash, hex_hash + 2);

    new_fp = fopen(write_path, "wb");
    if (!new_fp) { perror("fopen"); goto cleanup; }

    if (fwrite(compressed, 1, zlen, new_fp) != zlen) {
        perror("fwrite");
        goto cleanup;
    }

    if (flag && strcmp(flag, "w") == 0) {
        printf("%s\n", hex_hash);
    }

    // 6) Success: duplicate hash for caller ownership
    hash = (unsigned char *)malloc(20);
    if (!hash) { perror("malloc"); goto cleanup; }
    memcpy(hash, raw_hash, 20);

cleanup:
    if (fp) fclose(fp);
    if (new_fp) fclose(new_fp);
    if (blob) free(blob);
    if (compressed) free(compressed);
    return hash; // may be NULL if something failed
}



int main(int argc, char *argv[]) {
    // Disable output buffering
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 2) {
        fprintf(stderr, "Usage: ./your_program.sh <command> [<args>]\n");
        return 1;
    }
    
    const char *command = argv[1];
    
    if (strcmp(command, "init") == 0) {
        // You can use print statements as follows for debugging, they'll be visible when running tests.
        fprintf(stderr, "Logs from your program will appear here!\n");

        // Uncomment this block to pass the first stage
        // 
        if (mkdir(".git", 0755) == -1 || 
            mkdir(".git/objects", 0755) == -1 || 
            mkdir(".git/refs", 0755) == -1) {
            fprintf(stderr, "Failed to create directories: %s\n", strerror(errno));
            return 1;
        }
        
        FILE *headFile = fopen(".git/HEAD", "w");
        if (headFile == NULL) {
            fprintf(stderr, "Failed to create .git/HEAD file: %s\n", strerror(errno));
            return 1;
        }
        fprintf(headFile, "ref: refs/heads/main\n");
        fclose(headFile);
        
        printf("Initialized git directory\n");
    } else if ((strcmp(command, "cat-file") == 0)){
        const char *arg_two = argv[3]; // The object hash
        char path[256];
        build_path(path, sizeof(path), arg_two);

        FILE *fp = fopen(path, "rb");
        if (!fp) {
            perror("fopen");
            return 1;
        }

        // Get file size
        long compressed_size = get_file_size(fp);

        // Read compressed data into buffer
        unsigned char *compressed_data = malloc(compressed_size);
        fread(compressed_data, 1, compressed_size, fp);
        fclose(fp);

        // Prepare zlib stream
        z_stream stream = {0};
        inflateInit(&stream);
        stream.next_in = compressed_data;
        stream.avail_in = compressed_size;

        unsigned char buffer[16384]; // 16KB temp buffer
        int status;

        do {
            stream.next_out = buffer;
            stream.avail_out = sizeof(buffer);

            status = inflate(&stream, Z_NO_FLUSH);
            if (status == Z_STREAM_ERROR || status == Z_DATA_ERROR || status == Z_MEM_ERROR) {
                fprintf(stderr, "zlib inflate error: %d\n", status);
                inflateEnd(&stream);
                free(compressed_data);
                return 1;
            }

            fwrite(buffer + 8, 1, sizeof(buffer) - stream.avail_out - 8, stdout);
        } while (status != Z_STREAM_END);

        inflateEnd(&stream);
        free(compressed_data);

    } else if ((strcmp(command, "hash-object") == 0)){
        // Open file
        // ./your_program.sh hash-object -w test.txt

        unsigned char *hash = hash_blob_object(argv[3], "w");
        free(hash);
        
    } else if ((strcmp(command, "ls-tree") == 0)){
        // Example use: /path/to/your_program.sh ls-tree --name-only <tree_sha>
        const char *tree_sha = argv[3];
        char hex_hash[41]; 
        // hash_to_hex(hex_hash, tree_sha);
        strncpy(hex_hash, tree_sha, 40);
        hex_hash[40] = '\0';
        
        char path[256];
        build_path(path, sizeof(path), hex_hash);
        
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            perror("fopen");
            return 1;
        }

        long file_size = get_file_size(fp);
        unsigned char *compressed_data = malloc(file_size);
        fread(compressed_data, 1, file_size, fp);
        fclose(fp);

        unsigned char data[16384]; // 16KB buffer
        int decompressed_size = decompress_data(data, compressed_data, file_size);
        if (decompressed_size < 0) {
            fprintf(stderr, "Failed to decompress data.\n");
            free(compressed_data);
            return 1;
        }


        Tree tree = { NULL, 0 };
        unsigned char *null_position = memchr(data, '\0', sizeof(data) - 1);

        if (null_position) {
            size_t offset = null_position - data + 1;
            while (offset < decompressed_size) {
                // 1. Parse mode 
                char mode[7];
                size_t mode_len = 0;
                while (data[offset] != ' ') mode[mode_len++] = data[offset++];
                mode[mode_len] = '\0';
                offset++; //skip the space

                // 2. Parse filename
                size_t name_len = 0;
                while (data[offset + name_len] != '\0') name_len++;
                char *file_name = malloc(name_len + 1);
                memcpy(file_name, &data[offset], name_len);
                file_name[name_len] = '\0';
                offset += name_len + 1; // skip file name & null terminator

                // 3. Parse raw SHA1
                unsigned char raw_hash[20];
                memcpy(raw_hash, &data[offset], 20);
                offset += 20; // skip the saved raw_hash

                // 4. Store the Entry
                tree.entries = realloc(tree.entries, sizeof(Entry) * (tree.count + 1));
                Entry *entry = &tree.entries[tree.count++]; 
                strcpy(entry->mode, mode);
                entry->file_name = file_name;
                memcpy(entry->raw_hash, raw_hash, 20);
            }
        } 

        for (int i = 0; i < tree.count; i++) {
            Entry *entry = &tree.entries[i];
            printf("%s\n", entry->file_name);
            free(entry->file_name);
        }

        free(compressed_data);
        free(tree.entries); 


    } else if ((strcmp(command, "write-tree") == 0)){
        // Example use: /path/to/your_program.sh write-tree
        Tree tree = { NULL, 0 };
        unsigned char tree_hash[20];
        create_tree_object(".", &tree, tree_hash); 
        for (int i = 0; i < 20; i++) {
            printf("%02x", tree_hash[i]);
        }
        printf("\n");

    } else if ((strcmp(command, "commit-tree") == 0)) {
        // Surprisingly easier than previous two. It's just like hash-object but with a different type in header and body following the commit template seen with git cat-file -p HEAD (shows head commit).
        // new -p old
        // $ git commit-tree 5b825dc642cb6eb9a060e54bf8d69288fbee4904 -p 3b18e512dba79e4c8300dd08aeb37f8e728b8dad -m "Second commit"
        // $ ./your_program.sh commit-tree <tree_sha> -p <commit_sha> -m <message>
        char *tree_sha = argv[2];
        char *parent_sha = argv[4];
        char *message = argv[6];

        char *tree_label = "tree ";
        char *parent_label = "parent ";

        char *author = "author Zachary Marquez <his_email@stanford.edu> ";
        char *committer = "committer Mark Marquez <my_email@stanford.edu> ";

        time_t now = time(NULL); // seconds since epoch
        struct tm local_tm; 
        localtime_r(&now, &local_tm);
        long offset_seconds = local_tm.tm_gmtoff;
        int hours = (int)(offset_seconds / 3600);
        int minutes = (int)(labs(offset_seconds) % 3600) / 60;
        char time_str[64];
        snprintf(time_str, sizeof(time_str), "%ld %+03d%02d", (long)now, hours, minutes);

        size_t content_len= strlen(tree_label) + strlen(tree_sha) + 1;  // + 1 for newline character
        content_len += strlen(parent_label) + strlen(parent_sha) + 1; // + 1 for newline character
        content_len += strlen(author) + strlen(time_str) + 1;         // + 1 for newline character
        content_len += strlen(committer) + strlen(time_str) + 1;      // + 1 for newline character
        content_len += 1;                                             // + 1 for newline character
        content_len += strlen(message) + 1;                           // + 1 for newline character

        char header[64] = "commit ";
        snprintf(header + strlen(header), sizeof(header) - strlen(header), "%zu", content_len); 
        size_t header_len = strlen(header);

        size_t blob_len = header_len + 1 + content_len;
        unsigned char *blob = malloc(blob_len);

        // Copy header + null separator
        memcpy(blob, header, header_len);
        blob[header_len] = '\0'; // Required null between header and content

        unsigned char *p = blob + header_len + 1;
        p += sprintf((char *)p, "%s%s\n", tree_label, tree_sha);
        p += sprintf((char *)p, "%s%s\n", parent_label, parent_sha);
        p += sprintf((char *)p, "%s%s\n", author, time_str);
        p += sprintf((char *)p, "%s%s\n", committer, time_str);
        *p++ = '\n'; // blank line
        p += sprintf((char *)p, "%s\n", message);

        // Hash the uncompressed commit object
        unsigned char raw_hash[20];
        SHA1(blob, blob_len, raw_hash);
        char hex_hash[41];
        hash_to_hex(hex_hash, raw_hash);

        // Compress to zlib format (use compressBound to avoid overflow)
        uLongf cap = compressBound(blob_len);
        unsigned char *zbuf = malloc(cap);
        z_stream s = (z_stream){0};
        if (deflateInit(&s, Z_DEFAULT_COMPRESSION) != Z_OK) {
            fprintf(stderr, "deflateInit failed\n");
            free(blob);
            free(zbuf);
            return;
        }

        s.next_in  = blob;      s.avail_in  = (uInt)blob_len;
        s.next_out = zbuf;      s.avail_out = cap;

        int st = deflate(&s, Z_FINISH);
        size_t zlen = s.total_out;
        deflateEnd(&s);

        char dir_path[256];
        snprintf(dir_path, sizeof(dir_path), ".git/objects/%.2s", hex_hash);
        // mkdir may already exist; that's fine
        (void)mkdir(dir_path, 0755);

        char write_path[256];
        snprintf(write_path, sizeof(write_path), ".git/objects/%.2s/%.38s", hex_hash, hex_hash + 2);

        printf("%s", hex_hash);
     
    } else {
        fprintf(stderr, "Unknown command %s\n", command);
        return 1;
    }
    
    return 0;
}
