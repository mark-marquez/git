#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/sha.h>


typedef struct {
    char mode[7];               // e.g. "100644" is a file
    char *file_name;            // allocated dynamically
    unsigned char raw_hash[20];
} Entry; 


typedef struct {
    Entry *entries;
    size_t count;
} Tree;


void decompress_data(char *buffer, char *compressed_data, size_t compressed_size) {
    // Prepare zlib stream
    z_stream stream = {0};
    inflateInit(&stream);
    stream.next_in = compressed_data;
    stream.avail_in = compressed_size;

    int status;

    do {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);

        status = inflate(&stream, Z_NO_FLUSH);
        if (status == Z_STREAM_ERROR || status == Z_DATA_ERROR || status == Z_MEM_ERROR) {
            fprintf(stderr, "zlib inflate error: %d\n", status);
            inflateEnd(&stream);
            free(compressed_data);
        }

        fwrite(buffer + 8, 1, sizeof(buffer) - stream.avail_out - 8, stdout);
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
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
        char *path = argv[3];
        FILE *fp = fopen(path, "rb"); // need by to read binary data across OS's
        // if (!fp) {
        //     perror("fopen");
        //     return 1;
        // }
        
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
        // if (status != Z_STREAM_END) {
        //     fprintf(stderr, "Compression overflow.");
        //     free(compressed);
        //     return 1;
        // }
        deflateEnd(&stream);

        // Create file in .git/objects
        char dir_path[256];
        snprintf(dir_path, sizeof(dir_path), ".git/objects/%.2s", hex_hash);
        mkdir(dir_path, 0755);  // create parent dir if needed

        char write_path[256];
        snprintf(write_path, sizeof(write_path), ".git/objects/%.2s/%.38s", hex_hash, hex_hash + 2);
 

        FILE *new_fp = fopen(write_path, "wb");
        fwrite(compressed, 1, stream.total_out, new_fp);

        printf("%s\n", hex_hash);

        fclose(fp);
        fclose(new_fp);
        free(blob);
        free(compressed);
    } else if ((strcmp(command, "ls-tree") == 0)){
        // Example use: /path/to/your_program.sh ls-tree --name-only <tree_sha>
        const char *tree_sha = argv[3];
        char hex_hash[41]; 
        hash_to_hex(hex_hash, tree_sha);
        
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

        unsigned char data[16384]; // 16KB buffer
        decompress_data(data, compressed_data, file_size);

        Tree tree = { NULL, 0 };
        unsigned char *null_position = memchr(data, '\0', sizeof(data) - 1);

        if (null_position) {
            size_t offset = null_position - data + 1;
            while (offset < sizeof(data)) {
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

        fclose(fp);
        free(compressed_data);
        free(tree.entries); 

    } else {
        fprintf(stderr, "Unknown command %s\n", command);
        return 1;
    }
    
    return 0;
}
