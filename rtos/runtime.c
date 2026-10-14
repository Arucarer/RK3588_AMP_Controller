#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

void *memset(void *destination, int value, size_t count)
{
    /* 1. 逐字节填充目标区域。 */
    /* 2. 返回原始目标地址。 */

    unsigned char *dst = (unsigned char *)destination;

    while (count--)
        *dst++ = (unsigned char)value;

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    /* 1. 从源区域逐字节复制指定长度。 */
    /* 2. 返回原始目标地址，调用方须保证区域不重叠。 */

    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;

    while (count--)
        *dst++ = *src++;

    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    /* 1. 跳过零长度或源目标相同的情况。 */
    /* 2. 按地址关系选择正向或反向复制，避免重叠覆盖。 */
    /* 3. 返回原始目标地址。 */

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
    /* 1. 逐字节比较两个内存区域。 */
    /* 2. 首个不同字节决定返回值，全部相同则返回零。 */

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
    /* 1. 扫描字符串直到结束符。 */
    /* 2. 返回结束符之前的字节数。 */

    size_t length = 0;

    while (string[length] != '\0')
        length++;

    return length;
}

char *strcpy(char *destination, const char *source)
{
    /* 1. 复制源字符串并包含结束符。 */
    /* 2. 返回原始目标地址。 */

    char *dst = destination;

    while ((*dst++ = *source++) != '\0')
        ;

    return destination;
}

char *strncpy(char *destination, const char *source, size_t count)
{
    /* 1. 在长度上限内复制源字符串。 */
    /* 2. 源字符串较短时用零补齐剩余空间。 */
    /* 3. 返回原始目标地址。 */

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
    /* 1. 按顺序比较字符串字节。 */
    /* 2. 遇到差异、结束符或长度上限时停止并返回比较结果。 */

    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    /* 1. 按顺序比较字符串字节。 */
    /* 2. 遇到差异、结束符或长度上限时停止并返回比较结果。 */

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
