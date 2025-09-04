#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>

void generate_random_string(char *str, int length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    int charset_size = sizeof(charset) - 1;
    
    for (int i = 0; i < length; i++) {
        str[i] = charset[rand() % charset_size];
    }
    str[length] = '\0';
}


bool is_null_or_empty(const char *str) {
    return (str == NULL || strlen(str) == 0);
}