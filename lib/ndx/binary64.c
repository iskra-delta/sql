/*
 * Packs DBF numeric and date text into exact IEEE binary64 NDX keys.
 * The code uses small software big-integer helpers so the library does
 * not depend on host floating-point width or host floating-point behavior.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "binary64.h"

#include <string.h>

#define b64_max_limbs 80

typedef struct big_uint {
    unsigned short used;
    unsigned short limbs[b64_max_limbs];
} big_uint;

typedef struct decimal_number {
    int negative;
    int exponent10;
    big_uint digits;
} decimal_number;

static void clear_key(unsigned char *key)
{
    unsigned short index;

    for (index = 0; index < 8; index++) {
        key[index] = 0;
    }
}

static void big_zero(big_uint *value)
{
    unsigned short index;

    value->used = 0;
    for (index = 0; index < b64_max_limbs; index++) {
        value->limbs[index] = 0;
    }
}

static int big_is_zero(const big_uint *value)
{
    return value->used == 0;
}

static void big_normalize(big_uint *value)
{
    while (value->used > 0 && value->limbs[value->used - 1] == 0) {
        value->used--;
    }
}

static void big_copy(big_uint *target, const big_uint *source)
{
    unsigned short index;

    target->used = source->used;
    for (index = 0; index < source->used; index++) {
        target->limbs[index] = source->limbs[index];
    }
    for (; index < b64_max_limbs; index++) {
        target->limbs[index] = 0;
    }
}

static void big_from_u32(big_uint *value, unsigned long number)
{
    big_zero(value);
    if (number == 0UL) {
        return;
    }

    value->limbs[0] = (unsigned short)(number & 0xffffUL);
    value->limbs[1] = (unsigned short)((number >> 16) & 0xffffUL);
    value->used = value->limbs[1] != 0 ? 2 : 1;
}

static int big_add_small(big_uint *value, unsigned short addend)
{
    unsigned long carry;
    unsigned short index;

    if (addend == 0) {
        return 0;
    }
    if (value->used == 0) {
        value->limbs[0] = addend;
        value->used = 1;
        return 0;
    }

    carry = addend;
    index = 0;
    while (carry != 0UL && index < value->used) {
        carry = (unsigned long)value->limbs[index] + carry;
        value->limbs[index] = (unsigned short)(carry & 0xffffUL);
        carry >>= 16;
        index++;
    }
    if (carry != 0UL) {
        if (value->used >= b64_max_limbs) {
            return -1;
        }
        value->limbs[value->used++] = (unsigned short)carry;
    }

    return 0;
}

static int big_mul_small(big_uint *value, unsigned short multiplier)
{
    unsigned long carry;
    unsigned short index;
    unsigned long product;

    if (big_is_zero(value) || multiplier == 1U) {
        return 0;
    }
    if (multiplier == 0U) {
        big_zero(value);
        return 0;
    }

    carry = 0UL;
    for (index = 0; index < value->used; index++) {
        product = (unsigned long)value->limbs[index]
            * (unsigned long)multiplier + carry;
        value->limbs[index] = (unsigned short)(product & 0xffffUL);
        carry = product >> 16;
    }
    while (carry != 0UL) {
        if (value->used >= b64_max_limbs) {
            return -1;
        }
        value->limbs[value->used++] = (unsigned short)(carry & 0xffffUL);
        carry >>= 16;
    }

    return 0;
}

static unsigned short big_bit_length(const big_uint *value)
{
    unsigned short bits;
    unsigned short top;

    if (value->used == 0) {
        return 0;
    }

    bits = (unsigned short)((value->used - 1U) * 16U);
    top = value->limbs[value->used - 1U];
    while (top != 0U) {
        bits++;
        top >>= 1;
    }
    return bits;
}

static int big_shift_left_bits(big_uint *target, const big_uint *source,
    unsigned short bits)
{
    unsigned short word_shift;
    unsigned short bit_shift;
    unsigned short index;
    unsigned long carry;
    unsigned long part;
    unsigned short out;

    big_zero(target);
    if (source->used == 0) {
        return 0;
    }

    word_shift = (unsigned short)(bits / 16U);
    bit_shift = (unsigned short)(bits % 16U);
    if ((unsigned short)(source->used + word_shift + 1U) > b64_max_limbs) {
        return -1;
    }

    carry = 0UL;
    for (index = 0; index < source->used; index++) {
        part = ((unsigned long)source->limbs[index] << bit_shift) | carry;
        out = (unsigned short)(index + word_shift);
        target->limbs[out] = (unsigned short)(part & 0xffffUL);
        carry = part >> 16;
    }

    target->used = (unsigned short)(source->used + word_shift);
    if (carry != 0UL) {
        if (target->used >= b64_max_limbs) {
            return -1;
        }
        target->limbs[target->used++] = (unsigned short)carry;
    }
    big_normalize(target);
    return 0;
}

static int big_shift_left_one_inplace(big_uint *value, unsigned short bit)
{
    unsigned short index;
    unsigned long carry;
    unsigned long part;

    if (value->used == 0) {
        if (bit != 0U) {
            value->limbs[0] = bit;
            value->used = 1;
        }
        return 0;
    }

    carry = bit;
    for (index = 0; index < value->used; index++) {
        part = ((unsigned long)value->limbs[index] << 1) | carry;
        value->limbs[index] = (unsigned short)(part & 0xffffUL);
        carry = part >> 16;
    }
    if (carry != 0UL) {
        if (value->used >= b64_max_limbs) {
            return -1;
        }
        value->limbs[value->used++] = (unsigned short)carry;
    }
    return 0;
}

static void big_shift_right_one_inplace(big_uint *value)
{
    unsigned short index;
    unsigned short carry;
    unsigned short next_carry;

    carry = 0U;
    for (index = value->used; index > 0; index--) {
        next_carry = (unsigned short)(value->limbs[index - 1U] & 1U);
        value->limbs[index - 1U] = (unsigned short)(
            (value->limbs[index - 1U] >> 1) | (carry << 15));
        carry = next_carry;
    }
    big_normalize(value);
}

static int big_compare(const big_uint *left, const big_uint *right)
{
    unsigned short index;

    if (left->used < right->used) {
        return -1;
    }
    if (left->used > right->used) {
        return 1;
    }

    for (index = left->used; index > 0; index--) {
        if (left->limbs[index - 1U] < right->limbs[index - 1U]) {
            return -1;
        }
        if (left->limbs[index - 1U] > right->limbs[index - 1U]) {
            return 1;
        }
    }

    return 0;
}

static void big_subtract_inplace(big_uint *left, const big_uint *right)
{
    unsigned long borrow;
    unsigned short index;
    unsigned long lhs;
    unsigned long rhs;

    borrow = 0UL;
    for (index = 0; index < left->used; index++) {
        lhs = (unsigned long)left->limbs[index];
        rhs = borrow;
        if (index < right->used) {
            rhs += (unsigned long)right->limbs[index];
        }

        if (lhs < rhs) {
            left->limbs[index] = (unsigned short)(lhs + 0x10000UL - rhs);
            borrow = 1UL;
        } else {
            left->limbs[index] = (unsigned short)(lhs - rhs);
            borrow = 0UL;
        }
    }

    big_normalize(left);
}

static unsigned short big_get_bit(const big_uint *value, unsigned short bit)
{
    unsigned short word;
    unsigned short offset;

    word = (unsigned short)(bit / 16U);
    offset = (unsigned short)(bit % 16U);
    if (word >= value->used) {
        return 0U;
    }
    return (unsigned short)((value->limbs[word] >> offset) & 1U);
}

static int big_divide(const big_uint *numerator, const big_uint *denominator,
    big_uint *quotient, big_uint *remainder)
{
    unsigned short bit_count;
    unsigned short bit_index;

    if (big_is_zero(denominator)) {
        return -1;
    }

    big_zero(quotient);
    big_zero(remainder);
    bit_count = big_bit_length(numerator);
    for (bit_index = bit_count; bit_index > 0; bit_index--) {
        if (big_shift_left_one_inplace(remainder,
            big_get_bit(numerator, (unsigned short)(bit_index - 1U))) != 0) {
            return -1;
        }
        if (big_shift_left_one_inplace(quotient, 0U) != 0) {
            return -1;
        }
        if (big_compare(remainder, denominator) >= 0) {
            big_subtract_inplace(remainder, denominator);
            if (big_add_small(quotient, 1U) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int parse_exponent(const char *text, unsigned short length,
    unsigned short *index, int *exponent)
{
    int negative;
    int value;
    int saw_digit;

    negative = 0;
    value = 0;
    saw_digit = 0;

    if (*index < length && text[*index] == '-') {
        negative = 1;
        (*index)++;
    } else if (*index < length && text[*index] == '+') {
        (*index)++;
    }

    while (*index < length && text[*index] >= '0'
        && text[*index] <= '9') {
        saw_digit = 1;
        value = (value * 10) + (text[*index] - '0');
        (*index)++;
    }

    if (!saw_digit) {
        return -1;
    }

    *exponent = negative ? -value : value;
    return 0;
}

static int parse_decimal_number(const char *text, unsigned short length,
    decimal_number *number)
{
    unsigned short index;
    int after_point;
    int saw_digit;
    int frac_digits;
    int exponent;

    number->negative = 0;
    number->exponent10 = 0;
    big_zero(&number->digits);

    index = 0;
    while (index < length && text[index] == ' ') {
        index++;
    }
    if (index == length) {
        return 0;
    }

    if (text[index] == '-') {
        number->negative = 1;
        index++;
    } else if (text[index] == '+') {
        index++;
    }

    after_point = 0;
    saw_digit = 0;
    frac_digits = 0;
    exponent = 0;

    while (index < length) {
        if (text[index] >= '0' && text[index] <= '9') {
            saw_digit = 1;
            if (big_mul_small(&number->digits, 10U) != 0
                || big_add_small(&number->digits,
                    (unsigned short)(text[index] - '0')) != 0) {
                return -1;
            }
            if (after_point) {
                frac_digits++;
            }
            index++;
            continue;
        }

        if (text[index] == '.' && !after_point) {
            after_point = 1;
            index++;
            continue;
        }

        if ((text[index] == 'e' || text[index] == 'E')
            && index + 1U < length) {
            index++;
            if (parse_exponent(text, length, &index, &exponent) != 0) {
                return -1;
            }
            break;
        }

        if (text[index] == ' ') {
            while (index < length && text[index] == ' ') {
                index++;
            }
            break;
        }

        return -1;
    }

    while (index < length) {
        if (text[index] != ' ') {
            return -1;
        }
        index++;
    }

    if (!saw_digit || big_is_zero(&number->digits)) {
        number->negative = 0;
        number->exponent10 = 0;
        big_zero(&number->digits);
        return 0;
    }

    number->exponent10 = exponent - frac_digits;
    return 0;
}

static int build_ratio(big_uint *numerator, big_uint *denominator,
    const decimal_number *number)
{
    int power;

    big_copy(numerator, &number->digits);
    big_from_u32(denominator, 1UL);

    if (number->exponent10 >= 0) {
        for (power = 0; power < number->exponent10; power++) {
            if (big_mul_small(numerator, 10U) != 0) {
                return -1;
            }
        }
    } else {
        for (power = 0; power < -number->exponent10; power++) {
            if (big_mul_small(denominator, 10U) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int compare_ratio_power2(const big_uint *numerator,
    const big_uint *denominator, int exponent2)
{
    unsigned short numerator_bits;
    unsigned short denominator_bits;
    big_uint shifted;

    numerator_bits = big_bit_length(numerator);
    denominator_bits = big_bit_length(denominator);

    if (exponent2 >= 0) {
        if ((unsigned long)denominator_bits + (unsigned long)exponent2
            > (unsigned long)numerator_bits) {
            return -1;
        }
        if ((unsigned long)denominator_bits + (unsigned long)exponent2
            < (unsigned long)numerator_bits) {
            return 1;
        }
        if (big_shift_left_bits(&shifted, denominator,
            (unsigned short)exponent2) != 0) {
            return -1;
        }
        return big_compare(numerator, &shifted);
    }

    if ((unsigned long)numerator_bits + (unsigned long)(-exponent2)
        > (unsigned long)denominator_bits) {
        return 1;
    }
    if ((unsigned long)numerator_bits + (unsigned long)(-exponent2)
        < (unsigned long)denominator_bits) {
        return -1;
    }
    if (big_shift_left_bits(&shifted, numerator,
        (unsigned short)(-exponent2)) != 0) {
        return 1;
    }
    return big_compare(&shifted, denominator);
}

static int find_exponent2(const big_uint *numerator,
    const big_uint *denominator)
{
    int exponent2;

    exponent2 = (int)big_bit_length(numerator)
        - (int)big_bit_length(denominator);
    while (compare_ratio_power2(numerator, denominator, exponent2) < 0) {
        exponent2--;
    }
    while (compare_ratio_power2(numerator, denominator, exponent2 + 1) >= 0) {
        exponent2++;
    }
    return exponent2;
}

static int round_quotient(big_uint *quotient, const big_uint *remainder,
    const big_uint *denominator)
{
    big_uint twice_remainder;
    int compare;

    if (big_is_zero(remainder)) {
        return 0;
    }
    if (big_shift_left_bits(&twice_remainder, remainder, 1U) != 0) {
        return -1;
    }

    compare = big_compare(&twice_remainder, denominator);
    if (compare > 0 || (compare == 0 && big_get_bit(quotient, 0U) != 0U)) {
        if (big_add_small(quotient, 1U) != 0) {
            return -1;
        }
    }

    return 0;
}

static void pack_bits(unsigned char *key, int negative,
    unsigned short exponent_bits, const big_uint *significand)
{
    unsigned short bit;

    clear_key(key);
    for (bit = 0; bit < 52U; bit++) {
        if (big_get_bit(significand, bit) != 0U) {
            key[bit / 8U] = (unsigned char)(key[bit / 8U]
                | (unsigned char)(1U << (bit % 8U)));
        }
    }

    key[6] = (unsigned char)(key[6]
        | (unsigned char)((exponent_bits & 0x0fU) << 4));
    key[7] = (unsigned char)((exponent_bits >> 4) & 0x7fU);
    if (negative) {
        key[7] = (unsigned char)(key[7] | 0x80U);
    }
}

static int ratio_to_binary64(unsigned char *key, const big_uint *numerator,
    const big_uint *denominator, int negative)
{
    big_uint scaled_numerator;
    big_uint scaled_denominator;
    big_uint quotient;
    big_uint remainder;
    int exponent2;
    int shift;
    unsigned short exponent_bits;

    if (big_is_zero(numerator)) {
        clear_key(key);
        return 0;
    }

    exponent2 = find_exponent2(numerator, denominator);
    if (exponent2 > 1023) {
        return -1;
    }

    if (exponent2 >= -1022) {
        shift = 52 - exponent2;
        if (shift >= 0) {
            if (big_shift_left_bits(&scaled_numerator, numerator,
                (unsigned short)shift) != 0) {
                return -1;
            }
            big_copy(&scaled_denominator, denominator);
        } else {
            big_copy(&scaled_numerator, numerator);
            if (big_shift_left_bits(&scaled_denominator, denominator,
                (unsigned short)(-shift)) != 0) {
                return -1;
            }
        }

        if (big_divide(&scaled_numerator, &scaled_denominator, &quotient,
            &remainder) != 0
            || round_quotient(&quotient, &remainder,
                &scaled_denominator) != 0) {
            return -1;
        }
        if (big_bit_length(&quotient) > 53U) {
            big_shift_right_one_inplace(&quotient);
            exponent2++;
            if (exponent2 > 1023) {
                return -1;
            }
        }
        if (big_bit_length(&quotient) != 53U) {
            return -1;
        }

        exponent_bits = (unsigned short)(exponent2 + 1023);
        pack_bits(key, negative, exponent_bits, &quotient);
        return 0;
    }

    if (big_shift_left_bits(&scaled_numerator, numerator, 1074U) != 0) {
        return -1;
    }
    if (big_divide(&scaled_numerator, denominator, &quotient, &remainder)
        != 0 || round_quotient(&quotient, &remainder, denominator) != 0) {
        return -1;
    }

    if (big_is_zero(&quotient)) {
        clear_key(key);
        return 0;
    }
    if (big_bit_length(&quotient) > 52U) {
        big_zero(&quotient);
        pack_bits(key, negative, 1U, &quotient);
        return 0;
    }

    pack_bits(key, negative, 0U, &quotient);
    return 0;
}

static int date_to_julian(const char *text, unsigned short length,
    unsigned long *julian)
{
    static const unsigned char month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    unsigned short year;
    unsigned short month;
    unsigned short day;
    unsigned short index;
    unsigned long a;
    unsigned long y;
    unsigned long m;
    int leap;

    if (length != 8U) {
        return -1;
    }

    for (index = 0; index < 8U; index++) {
        if (text[index] == ' ') {
            *julian = 0UL;
            return 0;
        }
        if (text[index] < '0' || text[index] > '9') {
            return -1;
        }
    }

    year = (unsigned short)((text[0] - '0') * 1000
        + (text[1] - '0') * 100
        + (text[2] - '0') * 10
        + (text[3] - '0'));
    month = (unsigned short)((text[4] - '0') * 10 + (text[5] - '0'));
    day = (unsigned short)((text[6] - '0') * 10 + (text[7] - '0'));

    if (month == 0U || month > 12U || day == 0U) {
        return -1;
    }
    leap = ((year % 400U) == 0U)
        || (((year % 100U) != 0U) && ((year % 4U) == 0U));
    if (day > month_days[month - 1U]
        && !(month == 2U && day == 29U && leap)) {
        return -1;
    }

    a = (unsigned long)((14U - month) / 12U);
    y = (unsigned long)year + 4800UL - a;
    m = (unsigned long)month + (12UL * a) - 3UL;
    *julian = (unsigned long)day
        + ((153UL * m + 2UL) / 5UL)
        + (365UL * y)
        + (y / 4UL)
        - (y / 100UL)
        + (y / 400UL)
        - 32045UL;
    return 0;
}

static void pack_u32_exact(unsigned char *key, unsigned long value)
{
    unsigned short exponent2;
    unsigned short exponent_bits;
    unsigned short bit;
    unsigned short target_bit;

    clear_key(key);
    if (value == 0UL) {
        return;
    }

    exponent2 = 0U;
    while ((value >> (exponent2 + 1U)) != 0UL) {
        exponent2++;
    }

    exponent_bits = (unsigned short)(exponent2 + 1023U);
    for (bit = 0U; bit < 32U; bit++) {
        if ((value & (1UL << bit)) == 0UL) {
            continue;
        }
        target_bit = (unsigned short)(bit + 52U - exponent2);
        if (target_bit < 52U) {
            key[target_bit / 8U] = (unsigned char)(key[target_bit / 8U]
                | (unsigned char)(1U << (target_bit % 8U)));
        }
    }

    key[6] = (unsigned char)(key[6]
        | (unsigned char)((exponent_bits & 0x0fU) << 4));
    key[7] = (unsigned char)((exponent_bits >> 4) & 0x7fU);
}

static int key_is_zero(const unsigned char *key)
{
    return key[0] == 0U && key[1] == 0U && key[2] == 0U && key[3] == 0U
        && key[4] == 0U && key[5] == 0U && key[6] == 0U
        && (key[7] & 0x7fU) == 0U;
}

int ndx_binary64_from_numeric_text(unsigned char *key, const char *text,
    unsigned short length)
{
    decimal_number number;
    big_uint numerator;
    big_uint denominator;

    if (!key || !text) {
        return -1;
    }
    if (parse_decimal_number(text, length, &number) != 0) {
        return -1;
    }
    if (big_is_zero(&number.digits)) {
        clear_key(key);
        return 0;
    }
    if (build_ratio(&numerator, &denominator, &number) != 0) {
        return -1;
    }

    return ratio_to_binary64(key, &numerator, &denominator,
        number.negative);
}

int ndx_binary64_from_date_text(unsigned char *key, const char *text,
    unsigned short length)
{
    unsigned long julian;

    if (!key || !text) {
        return -1;
    }
    if (date_to_julian(text, length, &julian) != 0) {
        return -1;
    }
    pack_u32_exact(key, julian);
    return 0;
}

int ndx_binary64_compare(const unsigned char *left,
    const unsigned char *right)
{
    unsigned short index;
    int compare;
    int left_negative;
    int right_negative;

    if (key_is_zero(left) && key_is_zero(right)) {
        return 0;
    }

    left_negative = (left[7] & 0x80U) != 0U;
    right_negative = (right[7] & 0x80U) != 0U;
    if (left_negative != right_negative) {
        return left_negative ? -1 : 1;
    }

    compare = 0;
    for (index = 8U; index > 0U; index--) {
        if (left[index - 1U] < right[index - 1U]) {
            compare = -1;
            break;
        }
        if (left[index - 1U] > right[index - 1U]) {
            compare = 1;
            break;
        }
    }

    return left_negative ? -compare : compare;
}
