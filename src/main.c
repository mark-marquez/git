#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/sha.h>
#include <dirent.h>


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

// Caller provides a 20-byte buffer for the resulting hash
void create_tree_object(const char *dirpath, Tree *tree, unsigned char tree_hash[41]) {
    DIR *dir = opendir(dirpath);
    if (!dir) { perror("opendir"); return; }

    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0 || strcmp(dent->d_name, ".git") == 0)
            continue;
        
        char *file_name = malloc(strlen(dent->d_name) + 1);
        strcpy(file_name, dent->d_name);

        // Depend on whether entry is directory or file
        char mode[7];
        unsigned char raw_hash[20];

        if (dent->d_type == DT_DIR) { // FILE
            strcpy(mode, "100644");
            unsigned char* hash = hash_blob_object(file_name, ""); // flag is not "w"
            memcpy(raw_hash, hash, 20);
            free(hash);
        } else if (dent->d_type == DT_REG) { // DIRECTORY
            strcpy(mode, "40000"); 
            
            char subpath[PATH_MAX];
            snprintf(subpath, sizeof(subpath), "%s/%s", dirpath, dent->d_name);
            Tree new_tree = { NULL, 0 };
            unsigned char new_tree_hash[20];
            create_tree_object(subpath, &new_tree, new_tree_hash); 
            memcpy(raw_hash, new_tree_hash, 20);
        } else {
            continue; 
        }

        // 4. Store the Entry
        tree->entries = realloc(tree->entries, sizeof(Entry) * (tree->count + 1));
        Entry *entry = &tree->entries[tree->count++]; 
        strcpy(entry->mode, mode);
        entry->file_name = file_name;
        memcpy(entry->raw_hash, raw_hash, 20);
    }


    size_t tree_size = 0;
    Entry *entry = tree->entries;
    for (int i = 0; i < tree->count; i++) {
        tree_size += strlen(entry->mode) + 1;   // mode + space
        tree_size += strlen(entry->file_name) + 1;  // name + null
        tree_size += 20;                        // raw SHA
    }

    char header[64];
    int header_len = sprintf(header, "tree %zu", tree_size) + 1; // +1 for \0
    
    size_t buf_size = header_len + tree_size;
    unsigned char *tree_data = malloc(buf_size);
    memcpy(tree_data, header, header_len);

    unsigned char *p = tree_data + header_len;
    for (int i = 0; i < tree->count; i++) {
        Entry *e = &tree->entries[i];
        p += sprintf((char *)p, "%s %s", e->mode, e->file_name) + 1;
        memcpy(p, e->raw_hash, 20);
        p += 20;
    }

    // 2) hash (uncompressed tree object data)
    unsigned char sha[20];
    SHA1(tree_data, buf_size, sha);

    hash_to_hex(tree_hash, sha);

    // 4) compress
    uLongf cap = compressBound(buf_size);
    unsigned char *zbuf = malloc(cap);
    z_stream s = {0};
    deflateInit(&s, Z_DEFAULT_COMPRESSION);
    s.next_in  = tree_data; s.avail_in  = buf_size;
    s.next_out = zbuf;      s.avail_out = cap;
    int st = deflate(&s, Z_FINISH);
    deflateEnd(&s);
    size_t zlen = s.total_out;


    // 5) write to .git/objects/xx/yyyy...
    char dir[64], path[128];
    snprintf(dir,  sizeof(dir), ".git/objects/%.2s", tree_hash);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), ".git/objects/%.2s/%.38s", tree_hash, tree_hash+2);
    FILE *fp = fopen(path, "wb");
    fwrite(zbuf, 1, zlen, fp);
    fclose(fp);
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
    char *path = file_name;
    FILE *fp = fopen(path, "rb"); // need by to read binary data across OS's
    if (!fp) {
        perror("fopen");
        return 1;
    }
    
    // Get file size
    long file_length = get_file_size(fp);

    // Construct header
    char header[20] = "blob ";
    sprintf(header + strlen(header), "%ld", file_length); 
    size_t header_length = strlen(header);

    // Create header + content blob object
    size_t total_size = header_length + 1 + file_length;
    unsigned char *blob = malloc(total_size);
    memcpy(blob, header, header_length);
    blob[header_length] = '\0';
    fread(blob + header_length + 1, 1, file_length, fp);

    // Compute hash
    // unsigned char *SHA1(const unsigned char *data, size_t count, unsigned char *md_buf);
    unsigned char raw_hash[20];
    SHA1(blob, total_size, raw_hash);
    char hex_hash[41];
    hash_to_hex(hex_hash, raw_hash); 
    
    // Compress data using zlib
    z_stream stream = {0};
    deflateInit(&stream, Z_DEFAULT_COMPRESSION);

    unsigned char *compressed = malloc(total_size);
    stream.next_in = blob;
    stream.avail_in = total_size;
    stream.next_out = compressed;
    stream.avail_out = total_size;

    int status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        fprintf(stderr, "Compression overflow.");
        free(compressed);
        return 1;
    }
    deflateEnd(&stream);

    // Create file in .git/objects
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), ".git/objects/%.2s", hex_hash);
    mkdir(dir_path, 0755);  // create parent dir if needed

    char write_path[256];
    snprintf(write_path, sizeof(write_path), ".git/objects/%.2s/%.38s", hex_hash, hex_hash + 2);


    FILE *new_fp = fopen(write_path, "wb");
    fwrite(compressed, 1, stream.total_out, new_fp);

    if (strcmp(flag, "w") == 0) {
        printf("%s\n", hex_hash);
    }

    fclose(fp);
    fclose(new_fp);
    free(blob);
    free(compressed);

    unsigned char *hash = malloc(20);
    memcpy(hash, raw_hash, 20);
    return hash;
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
        // // Example use: /path/to/your_program.sh write-tree
        // Tree tree = { NULL, 0 };
        // char tree_hash[41];
        // create_tree_object(".", &tree, tree_hash); 
        // printf("%s\n", tree_hash);
    } else {
        fprintf(stderr, "Unknown command %s\n", command);
        return 1;
    }
    
    return 0;
}
