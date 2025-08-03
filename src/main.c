#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/sha.h>


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
        snprintf(path, sizeof(path), ".git/objects/%.2s/%.38s", arg_two, arg_two + 2);

        FILE *fp = fopen(path, "rb");
        if (!fp) {
            perror("fopen");
            return 1;
        }

        // Get file size
        fseek(fp, 0, SEEK_END);
        long compressed_size = ftell(fp);
        rewind(fp);

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
        fseek(fp, 0, SEEK_END);
        long file_length = ftell(fp);
        rewind(fp);

        // Construct header
        char header[20] = "blob ";
        sprintf(header + strlen(header), "%ld", file_length); 
        size_t header_length = strlen(header);
        header[header_length] = '\0';

        // Create header + content blob object
        size_t total_size = header_length + file_length;
        unsigned char *blob = malloc(total_size);
        memcpy(blob, header, header_length);
        fread(blob + header_length, 1, file_length, fp);

        // Compute hash
        // unsigned char *SHA1(const unsigned char *data, size_t count, unsigned char *md_buf);
        unsigned char hash[41];
        SHA1(blob, total_size, hash);
        hash[40] = '\0';
        
        // Compress data using zlib
        z_stream stream = {0};
        deflateInit(&stream, Z_DEFAULT_COMPRESSION);

        unsigned char *compressed = malloc(total_size);
        stream.next_out = compressed;
        stream.avail_out = total_size;

        int status = deflate(&stream, Z_FINISH);
        // if (status != Z_STREAM_END) {
        //     fprintf(stderr, "Compression overflow.");
        //     free(compressed);
        //     return 1;
        // }

        // Create file in .git/objects
        char write_path[256];
        snprintf(write_path, sizeof(path), ".git/objects/%.2s/%.38s", hash, hash + 2);

        FILE *new_fp = fopen(write_path, "wb");
        fwrite(compressed, 1, stream.total_out, new_fp);

        printf(hash);

        fclose(fp);
        free(blob);
        free(compressed);
    } else {
        fprintf(stderr, "Unknown command %s\n", command);
        return 1;
    }
    
    return 0;
}
