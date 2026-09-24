#include <stddef.h>
#include <stdint.h>

void *memset(void *destination, int value, size_t count)
{
    unsigned char *dst = (unsigned char *)destination;

    while (count--)
        *dst++ = (unsigned char)value;

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;

    while (count--)
        *dst++ = *src++;

    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;

    if (count == 0 || destination == source)
        return destination;

    if ((uintptr_t)destination < (uintptr_t)source) {
        while (count--)
            *dst++ = *src++;
    } else {
        dst += count;
        src += count;

        while (count--)
            *--dst = *--src;
    }

    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;

    while (count--) {
        if (*a != *b)
            return (int)*a - (int)*b;

        a++;
        b++;
    }

    return 0;
}

size_t strlen(const char *string)
{
    size_t length = 0;

    while (string[length] != '\0')
        length++;

    return length;
}

char *strcpy(char *destination, const char *source)
{
    char *dst = destination;

    while ((*dst++ = *source++) != '\0')
        ;

    return destination;
}

char *strncpy(char *destination, const char *source, size_t count)
{
    size_t i = 0;

    while (i < count && source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }

    while (i < count)
        destination[i++] = '\0';

    return destination;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }

    return (int)(unsigned char)*left -
           (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    while (count--) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;

        if (a != b)
            return (int)a - (int)b;

        if (a == '\0')
            return 0;
    }

    return 0;
}