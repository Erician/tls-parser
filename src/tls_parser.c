#include "tls_parser.h"

#define MIN_RECORD_LAYER_SIZE 3 // Has to be atleast (ContentType + TLS version)
#define MIN_CLIENT_HELLO_SIZE 38 // A client hello has to be atleast 38 bytes
#define MIN_SERVER_HELLO_SIZE 38 // A server hello has to be atleast 38 bytes
#define HELLO_RANDOM_BYTES_SIZE 28 // As specified in RFC
#define MAXIMUM_FILE_SIZE 20000000 // bytes => 20 MB

int main(int argc, char* argv[]) {
    int err = 0;

    // Check command line parameters and print usages in case they are not valid
    if (argc != 2) {
        printf("usage: %s\n path_to_file", argv[0]);

        return 0;
    }

    unsigned char *buf;
    int file_size = -1;

    // Check whether the path provided links to a reguler file and checks it's size
    if (((buf = get_safe_input_file(argv[1], &file_size)) == NULL) || (file_size == -1)) {
        return 0;
    }

    // Parse the record layer headers and save the actual handshake message into tls_message->body
    HandshakeMessage tls_message;
    memset(&tls_message, 0, sizeof(tls_message));
    err = initialize_tls_structure(buf, file_size, &tls_message);

    // Close the original buffer containing the file stream, as all data has to be in tls_message
    if (buf) {
        free(buf);
    }

    // Stop processing in case there was an error
    handle_errors(err);

    print_tls_record_layer_info(&tls_message);

    // Process the actual handshake message
    switch (tls_message.hsType) {
        case 1: 
            err = parse_client_hello(tls_message.body, tls_message.mLength); break;
        case 2:
            err = parse_server_hello(tls_message.body, tls_message.mLength); break;
        case 11:
            err = parse_certificate(tls_message.mLength); break;
        case 12: 
            err = parse_server_key_exchange(tls_message.mLength); break;
        case 14:
            err = parse_server_hello_done(tls_message.mLength); break;
        case 16:
            err = parse_client_key_exchange(tls_message.body, tls_message.mLength); break;
        default:
            err = UNSUPPORTED_MESSAGE_TYPE; break;
    }

    if (tls_message.body) {
        free(tls_message.body);
    }

    handle_errors(err);

    printf("\n[OK]: Finished parsing of message!\n");

    return 0;
}

int initialize_tls_structure(unsigned char *raw, int size, HandshakeMessage *tls_message) {
    // Record layer
    if (size <= MIN_RECORD_LAYER_SIZE || raw == NULL) {
        return INVALID_FILE_LENGTH;
    }

    int pos = 0;

    // Only handshake messages of TLS version 1.0 - 1.2 are allowed
    if (raw[pos++] != HANDSHAKE) {
        return INVALID_CONTENT_TYPE;
    }

    if (!is_valid_tls_version(raw[pos], raw[pos + 1])) {
        return INVALID_VERSION;
    }

    // Values are safe to assign to our structure
    tls_message->cType = HANDSHAKE;
    tls_message->version.major = raw[1];
    tls_message->version.minor = raw[2];

    pos += 2;

    // Convert raw[3] and raw[4] to uint16_t number
    tls_message->fLength = (raw[pos] << 8) + raw[pos + 1];
    pos += 2;

    // Check if the sizes are correct (record protocol headers + length == file size)
    if (tls_message->fLength + pos != size) {
        return INVALID_FILE_LENGTH;
    }

    // Does not need to check this value as the parser will not continue if this is not a supported handshake message type
    tls_message->hsType = raw[pos++];

    // Convert raw[6], raw[7] and raw[8] into uint24_t number
    // It's actually uint24_t but thats not defined
    tls_message->mLength = (0x00 << 24) + (raw[pos] << 16) + (raw[pos + 1] << 8) + raw[pos + 2];
    pos += 3;

    // Check if the sizes are correct (fLength value == mLength value + HandshakeType (1 byte) + mLength (3 bytes))
    if (tls_message->fLength != tls_message->mLength + 4) {
        return INVALID_FILE_LENGTH;
    }

    // Copy the rest of the message into our structure, so we can close the raw stream
    tls_message->body = (unsigned char *)malloc(tls_message->mLength);
    memcpy(tls_message->body, raw + pos, tls_message->mLength);

    return 0;
}

void print_tls_record_layer_info(HandshakeMessage *tls_message) {
    printf("Identified the following TLS message:\n\n");
    printf("TLS Version: ");

    print_tls_version(tls_message->version.minor);

    printf("Protocol type: %d\n", tls_message->cType);
    printf("Fragment length: %d\n", tls_message->fLength);
    printf("Handshake message type: %d\n\n", tls_message->hsType);
}

int parse_client_hello(unsigned char *message, uint16_t size) {
    if (size < MIN_CLIENT_HELLO_SIZE || message == NULL) {
        return INVALID_FILE_LENGTH;
    }

    int pos = 0;

    ClientHello client_hello;
    memset(&client_hello, 0, sizeof(client_hello));

    // Check if the versions are valid
    if (!is_valid_tls_version(message[pos], message[pos + 1])) {
            return INVALID_VERSION;
    }

    client_hello.version.major = message[pos];
    client_hello.version.minor = message[pos + 1];
    pos += 2;

    // The Random structure    
    client_hello.random.time = (message[pos] << 24) + (message[pos + 1] << 16) + (message[pos + 2] << 8) + message[pos + 3];
    pos += 4;
    memcpy(client_hello.random.random_bytes, message + pos, HELLO_RANDOM_BYTES_SIZE);
    pos += HELLO_RANDOM_BYTES_SIZE;

    // The SessionID structure
    client_hello.sessionId.length = message[pos++];
    if (client_hello.sessionId.length > 0) {
        if (size < client_hello.sessionId.length + HELLO_RANDOM_BYTES_SIZE + 4 + 2) {
            return INVALID_FILE_LENGTH;
        }

        client_hello.sessionId.sessionId = (unsigned char *)malloc(client_hello.sessionId.length);
        memcpy(client_hello.sessionId.sessionId, message + pos, client_hello.sessionId.length);
        pos += client_hello.sessionId.length;
    }

    // The CipherSuitesStructure
    client_hello.csCollection.length = (message[pos] << 8) + message[pos + 1];
    pos += 2;
    if (client_hello.csCollection.length > 0) {
        if (size < pos + client_hello.csCollection.length) {
            return INVALID_FILE_LENGTH;
        }

        client_hello.csCollection.cipherSuites = (unsigned char *)malloc(client_hello.csCollection.length);
        memcpy(client_hello.csCollection.cipherSuites, message + pos, client_hello.csCollection.length);
        pos += client_hello.csCollection.length;
    }

    // CompresionMethod 2 bytes and Extensions 1 at least
    if (size < pos + 3) {
            return INVALID_FILE_LENGTH;
    }

    // The CompresionMethodStructure
    client_hello.compresionMethod.length = message[pos++];
    if (client_hello.compresionMethod.length != 1) {
            printf("%x", client_hello.compresionMethod.length);
            return INVALID_FILE_LENGTH;
    }

    client_hello.compresionMethod.compresionMethod = message[pos++];

    if (size != pos) {
        // Extensions are present.
        // Save to rest of the data to our structue. No more checks about it,
        // we will just print it out as extensions are not in scope. 
        client_hello.hasExtensions = 1; 
        client_hello.extensions = (unsigned char *)malloc(size - pos);
        memcpy(client_hello.extensions, message + pos, size - pos);
    }

    print_client_hello_message(&client_hello, size - pos);

    clean_client_hello(client_hello);

    return 0;
}

void print_client_hello_message(ClientHello *message, int extensions_length) {
    printf("Details of ClientHello:\n\n");
    printf("TLS Version: ");

    print_tls_version(message->version.minor);

    // Time in human-readable format
    time_t raw_time = (time_t) message->random.time;
    struct tm *timeinfo = localtime(&raw_time);
    char buf[25];

    strftime(buf, 25, "Timestamp: %c.", timeinfo);
    puts(buf);

    printf("Random data: ");
    int i;
    for (i = 0; i < HELLO_RANDOM_BYTES_SIZE; i++) {        
        printf("%x", message->random.random_bytes[i]);
    }
    printf("\n");

    printf("SessionID: ");
    if ( message->sessionId.length != 0) {
        for (i = 0; i < message->sessionId.length; i++) { 
            printf("%x", message->sessionId.sessionId[i]);
        }
    } else {
        printf("N/A");
    }

    printf("\n");

    printf("Choosen cipher suites:\n");
    for (i = 0; i < message->csCollection.length; i++) {
        if (i % 2) {
            printf("%x ", message->csCollection.cipherSuites[i]); 
        } else {
            printf("0x%x", message->csCollection.cipherSuites[i]);
        }
    }
    printf("\n");

    printf("Compresion method: %d\n", message->compresionMethod.compresionMethod);
    printf("Has extensions: %s\n", message->hasExtensions ? "true" : "false");

    printf("Raw extensions data:\n\n");
    for (i = 0; i < extensions_length; i++) {
        printf("%x", message->extensions[i]);
    }

    printf("\n");
}

int parse_server_hello(unsigned char *message, uint16_t size) {
    if (size < MIN_SERVER_HELLO_SIZE || message == NULL) {
        return INVALID_FILE_LENGTH;
    }

    int pos = 0;

    ServerHello server_hello;
    memset(&server_hello, 0, sizeof(server_hello));

    // Check if the versions are valid
    if (!is_valid_tls_version(message[pos], message[pos + 1])) {
            return INVALID_VERSION;
    }

    server_hello.version.major = message[pos];
    server_hello.version.minor = message[pos + 1];
    pos += 2;

    // The Random structure    
    server_hello.random.time = (message[pos] << 24) + (message[pos + 1] << 16) + (message[pos + 2] << 8) + message[pos + 3];
    pos += 4;
    memcpy(server_hello.random.random_bytes, message + pos, HELLO_RANDOM_BYTES_SIZE);
    pos += HELLO_RANDOM_BYTES_SIZE;

    // The SessionID structure
    server_hello.sessionId.length = message[pos++];
    if (server_hello.sessionId.length > 0) {
        if (size < server_hello.sessionId.length + HELLO_RANDOM_BYTES_SIZE + 4 + 2) {
            return INVALID_FILE_LENGTH;
        }

        server_hello.sessionId.sessionId = (unsigned char *)malloc(server_hello.sessionId.length);
        memcpy(server_hello.sessionId.sessionId, message + pos, server_hello.sessionId.length);
        pos += server_hello.sessionId.length;
    }

    // The choosen cipher suite
    server_hello.cipherSuite[0] = message[pos++];
    server_hello.cipherSuite[1] = message[pos++];

    // CompresionMethod needs to be present
    if (size < pos + 1) {
            return INVALID_FILE_LENGTH;
    }

    // The CompresionMethodStructure
    server_hello.compresionMethod = message[pos++];
   
    if (size != pos) {
        // Extensions are present.
        // Save to rest of the data to our structue. No more checks about it,
        // we will just print it out as extensions are not in scope. 
        server_hello.hasExtensions = 1; 
        server_hello.extensions = (unsigned char *)malloc(size - pos);
        memcpy(server_hello.extensions, message + pos, size - pos);
    }

    print_server_hello_message(&server_hello, size - pos);

    clean_server_hello(server_hello);

    return 0;
}

void print_server_hello_message(ServerHello *message, int extensions_length) {
    printf("Details of ServerHello:\n\n");
    printf("TLS Version: ");

    print_tls_version(message->version.minor);

    // Time in human-readable format
    time_t raw_time = (time_t) message->random.time;
    struct tm *timeinfo = localtime(&raw_time);
    printf ("Timestamp: %s", asctime(timeinfo));

    printf("Random data: ");
    int i;
    for (i = 0; i < HELLO_RANDOM_BYTES_SIZE; i++) {        
        printf("%x", message->random.random_bytes[i]);
    }
    printf("\n");

    printf("SessionID: ");
    if ( message->sessionId.length != 0) {
        for (i = 0; i < message->sessionId.length; i++) { 
            printf("%x", message->sessionId.sessionId[i]);
        }
    } else {
        printf("N/A");
    }

    printf("\n");

    printf("Choosen cipher suite: 0x");
    printf("%x", message->cipherSuite[0]); 
    printf("%x\n", message->cipherSuite[1]);

    printf("Compresion method: %d\n", message->compresionMethod);
    if (message->hasExtensions) {
        printf("Has extensions: true\n");
        printf("Raw extensions data:\n\n");
        for (i = 0; i < extensions_length; i++) {
            printf("%x", message->extensions[i]);
        }
    } else {
        printf("Has extensions: false");
    } 

    printf("\n");
}

void print_tls_version(uint8_t minor) {
    switch (minor) {
        case 0x01: printf("1.0\n"); break;
        case 0x02: printf("1.1\n"); break;
        case 0x03: printf("1.2\n"); break;
        default: printf("unknown\n"); break;
    }
}

int parse_certificate(uint16_t size) {
    // The Certificate message contains only a chain of certificates. 
    // The only thing to do is to verify, that the chain is not empty 
    // as we are not able to (and not supposed to) say anything about the data.
    if (size == 0) {
        return INVALID_FILE_LENGTH;
    }

    printf("The certificate chain provided is %d bytes long.\n", size);
    
    return 0;
}

int parse_server_key_exchange( uint16_t size) {
    // The actual algorithm and other stuff like digital signatures of params
    // are not in scope as their presence is determined by extensions in hello messages
    // and the used certificate (which are both ignored).
    printf("The key exchange parameters provided are %d bytes long.\n", size);


    return 0;
}

int parse_server_hello_done(uint16_t size) {
    // The ServerHelloDone is empty. Just check if thats true.
    if (size != 0) {
        return INVALID_FILE_LENGTH;
    }

    return 0;
}

int parse_client_key_exchange(unsigned char *message, uint16_t size) {
    // We only check until we get to the exchange parameters, whose
    // type is specified similiary as server key exchange parameters
    // in earlier messages.
    uint8_t length = message[0];

    if (length != size - 1) {
        return INVALID_FILE_LENGTH_FOR_CLIENT_KEY_EXCHANGE;
    }

    printf("The key exchange parameters provided are %d bytes long.\n", size);

    return 0;
}

void clean_client_hello(ClientHello message) {
    if (message.sessionId.sessionId) {
        free(message.sessionId.sessionId);
    }

    if (message.csCollection.cipherSuites) {
        free(message.csCollection.cipherSuites);
    }

    if (message.extensions) {
        free(message.extensions);
    }
}

void clean_server_hello(ServerHello message) {
    if (message.sessionId.sessionId) {
        free(message.sessionId.sessionId);
    }

    if (message.extensions) {
        free(message.extensions);
    }
}

int is_valid_tls_version(unsigned char major, unsigned char minor) {
    return major == 0x03 && (minor == 0x01 || minor == 0x02 || minor == 0x03);
}

unsigned char* get_safe_input_file(char *path, int *file_size) {
    struct stat sb;

    // Only regular files are processed. No symbolic links, sockets, dirs, etc.
    if (lstat(path, &sb) == 0 && !S_ISREG(sb.st_mode))
    {
        printf("The path '%s' is not a regular file.\n", path);

        return NULL;
    }

    // Try to open provided file
    FILE *stream;
    stream = fopen(path, "rb");

    if (stream == NULL) {
        printf("The file '%s' couldn't be opened.\n", path);
        return NULL;
    }

    // Get the actual file length
    if (fseeko(stream, 0, SEEK_END) != 0) {
        printf("Couldn't read the file '%s' (fseeko).\n", path);
        fclose_safe(stream);

        return NULL;
    }

    *file_size = ftello(stream);
    if (*file_size == -1) {
        printf("Couldn't read the file '%s' (ftello).\n", path);
        fclose_safe(stream);

        return NULL;
    }

    // Prevent hangs when a very large file is given by the user
    if (*file_size > MAXIMUM_FILE_SIZE) {
        printf("The file '%s' is larger then 20 MB.\n", path);
        fclose_safe(stream);

        return NULL;
    }

    fseek(stream, 0, SEEK_SET);

    // Copy file content into buffer and close file stream
    unsigned char *buf = (unsigned char *)malloc(*file_size);
    fread(buf, *file_size, 1, stream);

    fclose_safe(stream);

    return buf;
}

void fclose_safe(FILE * stream) {
    if (stream != NULL) {
        fclose(stream);
    }
}

void handle_errors(int error_code) {
    if (!error_code) {
        // In case there is no error, continue.
        return;
    }

    printf("[ERROR]: ");

    switch (error_code) {
        case 1: printf("The lengths specified in the input file are not valid.\n"); break;
        case 2: printf("The input file is not an TLS handshake message.\n"); break;
        case 3: printf("The message is not of a supported version (TLS 1.0 - TLS 1.2).\n"); break;
        case 4: printf("Unsupported handshake message type.\n"); break;
	case 5: printf("The lengths specified in the input file are not valid for client_key_exchange message.\n"); break;
        default:
            printf("Something truly unexpected happend.\n"); break;
    }

    exit(0);
}
