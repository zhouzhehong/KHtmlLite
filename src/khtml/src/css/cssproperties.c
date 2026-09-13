/* ANSI-C code produced by gperf version 3.3 */
/* Command-line: gperf -a -L ANSI-C -E -c -C -o -t -k '*' -NfindProp -Hhash_prop -Wwordlist_prop -D -s 2 cssproperties.gperf  */

#if !((' ' == 32) && ('!' == 33) && ('"' == 34) && ('#' == 35) \
      && ('%' == 37) && ('&' == 38) && ('\'' == 39) && ('(' == 40) \
      && (')' == 41) && ('*' == 42) && ('+' == 43) && (',' == 44) \
      && ('-' == 45) && ('.' == 46) && ('/' == 47) && ('0' == 48) \
      && ('1' == 49) && ('2' == 50) && ('3' == 51) && ('4' == 52) \
      && ('5' == 53) && ('6' == 54) && ('7' == 55) && ('8' == 56) \
      && ('9' == 57) && (':' == 58) && (';' == 59) && ('<' == 60) \
      && ('=' == 61) && ('>' == 62) && ('?' == 63) && ('A' == 65) \
      && ('B' == 66) && ('C' == 67) && ('D' == 68) && ('E' == 69) \
      && ('F' == 70) && ('G' == 71) && ('H' == 72) && ('I' == 73) \
      && ('J' == 74) && ('K' == 75) && ('L' == 76) && ('M' == 77) \
      && ('N' == 78) && ('O' == 79) && ('P' == 80) && ('Q' == 81) \
      && ('R' == 82) && ('S' == 83) && ('T' == 84) && ('U' == 85) \
      && ('V' == 86) && ('W' == 87) && ('X' == 88) && ('Y' == 89) \
      && ('Z' == 90) && ('[' == 91) && ('\\' == 92) && (']' == 93) \
      && ('^' == 94) && ('_' == 95) && ('a' == 97) && ('b' == 98) \
      && ('c' == 99) && ('d' == 100) && ('e' == 101) && ('f' == 102) \
      && ('g' == 103) && ('h' == 104) && ('i' == 105) && ('j' == 106) \
      && ('k' == 107) && ('l' == 108) && ('m' == 109) && ('n' == 110) \
      && ('o' == 111) && ('p' == 112) && ('q' == 113) && ('r' == 114) \
      && ('s' == 115) && ('t' == 116) && ('u' == 117) && ('v' == 118) \
      && ('w' == 119) && ('x' == 120) && ('y' == 121) && ('z' == 122) \
      && ('{' == 123) && ('|' == 124) && ('}' == 125) && ('~' == 126))
/* The character set is not based on ISO-646.  */
#error "gperf generated tables don't work with this execution character set. Please report a bug to <bug-gperf@gnu.org>."
#endif

#line 1 "cssproperties.gperf"

#include "cssproperties.h"
struct css_prop { const char *name; int id; };
#line 5 "cssproperties.gperf"
struct css_prop;
/* maximum key range = 836, duplicates = 0 */

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
static unsigned int
hash_prop (register const char *str, register size_t len)
{
  static const unsigned short asso_values[] =
    {
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841,   0, 841, 841, 841, 841,
      841,   0, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841,   5,   0,   0,
        0,   0, 185,  15, 115,  20,   5,  70,   0,  20,
       20,   0,   0,  65,   0,  10,   5,  50,  30, 325,
       60, 290,  10, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841, 841, 841, 841, 841,
      841, 841, 841, 841, 841, 841
    };
  register unsigned int hval = len;

  switch (hval)
    {
      default:
        hval += asso_values[(unsigned char)str[31]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 31:
        hval += asso_values[(unsigned char)str[30]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 30:
        hval += asso_values[(unsigned char)str[29]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 29:
        hval += asso_values[(unsigned char)str[28]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 28:
        hval += asso_values[(unsigned char)str[27]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 27:
        hval += asso_values[(unsigned char)str[26]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 26:
        hval += asso_values[(unsigned char)str[25]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 25:
        hval += asso_values[(unsigned char)str[24]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 24:
        hval += asso_values[(unsigned char)str[23]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 23:
        hval += asso_values[(unsigned char)str[22]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 22:
        hval += asso_values[(unsigned char)str[21]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 21:
        hval += asso_values[(unsigned char)str[20]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 20:
        hval += asso_values[(unsigned char)str[19]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 19:
        hval += asso_values[(unsigned char)str[18]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 18:
        hval += asso_values[(unsigned char)str[17]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 17:
        hval += asso_values[(unsigned char)str[16]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 16:
        hval += asso_values[(unsigned char)str[15]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 15:
        hval += asso_values[(unsigned char)str[14]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 14:
        hval += asso_values[(unsigned char)str[13]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 13:
        hval += asso_values[(unsigned char)str[12]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 12:
        hval += asso_values[(unsigned char)str[11]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 11:
        hval += asso_values[(unsigned char)str[10]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 10:
        hval += asso_values[(unsigned char)str[9]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 9:
        hval += asso_values[(unsigned char)str[8]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 8:
        hval += asso_values[(unsigned char)str[7]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 7:
        hval += asso_values[(unsigned char)str[6]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 6:
        hval += asso_values[(unsigned char)str[5]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 5:
        hval += asso_values[(unsigned char)str[4]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 4:
        hval += asso_values[(unsigned char)str[3]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 3:
        hval += asso_values[(unsigned char)str[2]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 2:
        hval += asso_values[(unsigned char)str[1]];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 1:
        hval += asso_values[(unsigned char)str[0]];
        break;
    }
  return hval;
}

const struct css_prop *
findProp (register const char *str, register size_t len)
{
  enum
    {
      TOTAL_KEYWORDS = 186,
      MIN_WORD_LENGTH = 3,
      MAX_WORD_LENGTH = 32,
      MIN_HASH_VALUE = 5,
      MAX_HASH_VALUE = 840
    };

#if (defined __GNUC__ && __GNUC__ + (__GNUC_MINOR__ >= 6) > 4) || (defined __clang__ && __clang_major__ >= 3)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
  static const struct css_prop wordlist_prop[] =
    {
#line 42 "cssproperties.gperf"
      {"color", 36},
#line 119 "cssproperties.gperf"
      {"border", 113},
#line 107 "cssproperties.gperf"
      {"top", 101},
#line 40 "cssproperties.gperf"
      {"clear", 34},
#line 120 "cssproperties.gperf"
      {"border-color", 114},
#line 99 "cssproperties.gperf"
      {"src", 93},
#line 122 "cssproperties.gperf"
      {"border-top", 116},
#line 26 "cssproperties.gperf"
      {"border-top-color", 20},
#line 191 "cssproperties.gperf"
      {"gap", 185},
#line 41 "cssproperties.gperf"
      {"clip", 35},
#line 173 "cssproperties.gperf"
      {"stop-color", 167},
#line 22 "cssproperties.gperf"
      {"border-collapse", 16},
#line 38 "cssproperties.gperf"
      {"bottom", 32},
#line 124 "cssproperties.gperf"
      {"border-bottom", 118},
#line 98 "cssproperties.gperf"
      {"size", 92},
#line 28 "cssproperties.gperf"
      {"border-bottom-color", 22},
#line 133 "cssproperties.gperf"
      {"scrollbar-base-color", 127},
#line 43 "cssproperties.gperf"
      {"content", 37},
#line 46 "cssproperties.gperf"
      {"cursor", 40},
#line 132 "cssproperties.gperf"
      {"padding", 126},
#line 47 "cssproperties.gperf"
      {"direction", 41},
#line 87 "cssproperties.gperf"
      {"padding-top", 81},
#line 146 "cssproperties.gperf"
      {"clip-rule", 140},
#line 95 "cssproperties.gperf"
      {"position", 89},
#line 23 "cssproperties.gperf"
      {"border-spacing", 17},
#line 130 "cssproperties.gperf"
      {"margin", 124},
#line 150 "cssproperties.gperf"
      {"color-rendering", 144},
#line 175 "cssproperties.gperf"
      {"stroke", 169},
#line 39 "cssproperties.gperf"
      {"caption-side", 33},
#line 58 "cssproperties.gperf"
      {"letter-spacing", 52},
#line 63 "cssproperties.gperf"
      {"margin-top", 57},
#line 21 "cssproperties.gperf"
      {"border-radius", 15},
#line 166 "cssproperties.gperf"
      {"marker", 160},
#line 131 "cssproperties.gperf"
      {"outline", 125},
#line 45 "cssproperties.gperf"
      {"counter-reset", 39},
#line 89 "cssproperties.gperf"
      {"padding-bottom", 83},
#line 80 "cssproperties.gperf"
      {"outline-color", 74},
#line 170 "cssproperties.gperf"
      {"mask", 164},
#line 147 "cssproperties.gperf"
      {"color-interpolation", 141},
#line 139 "cssproperties.gperf"
      {"scrollbar-track-color", 133},
#line 117 "cssproperties.gperf"
      {"z-index", 111},
#line 65 "cssproperties.gperf"
      {"margin-bottom", 59},
#line 171 "cssproperties.gperf"
      {"pointer-events", 165},
#line 167 "cssproperties.gperf"
      {"marker-end", 161},
#line 188 "cssproperties.gperf"
      {"align-items", 182},
#line 169 "cssproperties.gperf"
      {"marker-start", 163},
#line 110 "cssproperties.gperf"
      {"vertical-align", 104},
#line 102 "cssproperties.gperf"
      {"text-decoration", 96},
#line 96 "cssproperties.gperf"
      {"quotes", 90},
#line 101 "cssproperties.gperf"
      {"text-align", 95},
#line 108 "cssproperties.gperf"
      {"unicode-bidi", 102},
#line 109 "cssproperties.gperf"
      {"unicode-range", 103},
#line 178 "cssproperties.gperf"
      {"stroke-linecap", 172},
#line 168 "cssproperties.gperf"
      {"marker-mid", 162},
#line 103 "cssproperties.gperf"
      {"text-indent", 97},
#line 163 "cssproperties.gperf"
      {"image-rendering", 157},
#line 164 "cssproperties.gperf"
      {"kerning", 158},
#line 145 "cssproperties.gperf"
      {"clip-path", 139},
#line 78 "cssproperties.gperf"
      {"orphans", 72},
#line 184 "cssproperties.gperf"
      {"text-rendering", 178},
#line 97 "cssproperties.gperf"
      {"right", 91},
#line 151 "cssproperties.gperf"
      {"dominant-baseline", 145},
#line 127 "cssproperties.gperf"
      {"box-sizing", 121},
#line 123 "cssproperties.gperf"
      {"border-right", 117},
#line 118 "cssproperties.gperf"
      {"background", 112},
#line 27 "cssproperties.gperf"
      {"border-right-color", 21},
#line 7 "cssproperties.gperf"
      {"background-color", 1},
#line 44 "cssproperties.gperf"
      {"counter-increment", 38},
#line 143 "cssproperties.gperf"
      {"alignment-baseline", 137},
#line 94 "cssproperties.gperf"
      {"page-break-inside", 88},
#line 179 "cssproperties.gperf"
      {"stroke-linejoin", 173},
#line 9 "cssproperties.gperf"
      {"background-repeat", 3},
#line 137 "cssproperties.gperf"
      {"scrollbar-3dlight-color", 131},
#line 57 "cssproperties.gperf"
      {"left", 51},
#line 14 "cssproperties.gperf"
      {"background-clip", 8},
#line 157 "cssproperties.gperf"
      {"flood-color", 151},
#line 50 "cssproperties.gperf"
      {"float", 44},
#line 125 "cssproperties.gperf"
      {"border-left", 119},
#line 152 "cssproperties.gperf"
      {"enable-background", 146},
#line 29 "cssproperties.gperf"
      {"border-left-color", 23},
#line 153 "cssproperties.gperf"
      {"fill", 147},
#line 180 "cssproperties.gperf"
      {"stroke-miterlimit", 174},
#line 128 "cssproperties.gperf"
      {"font", 122},
#line 16 "cssproperties.gperf"
      {"background-size", 10},
#line 156 "cssproperties.gperf"
      {"filter", 150},
#line 149 "cssproperties.gperf"
      {"color-profile", 143},
#line 172 "cssproperties.gperf"
      {"shape-rendering", 166},
#line 183 "cssproperties.gperf"
      {"text-anchor", 177},
#line 165 "cssproperties.gperf"
      {"lighting-color", 159},
#line 134 "cssproperties.gperf"
      {"scrollbar-face-color", 128},
#line 88 "cssproperties.gperf"
      {"padding-right", 82},
#line 8 "cssproperties.gperf"
      {"background-image", 2},
#line 64 "cssproperties.gperf"
      {"margin-right", 58},
#line 192 "cssproperties.gperf"
      {"flex", 186},
#line 15 "cssproperties.gperf"
      {"background-origin", 9},
#line 11 "cssproperties.gperf"
      {"background-position", 5},
#line 52 "cssproperties.gperf"
      {"font-size", 46},
#line 90 "cssproperties.gperf"
      {"padding-left", 84},
#line 155 "cssproperties.gperf"
      {"fill-rule", 149},
#line 17 "cssproperties.gperf"
      {"border-top-right-radius", 11},
#line 56 "cssproperties.gperf"
      {"height", 50},
#line 66 "cssproperties.gperf"
      {"margin-left", 60},
#line 18 "cssproperties.gperf"
      {"border-bottom-right-radius", 12},
#line 93 "cssproperties.gperf"
      {"page-break-before", 87},
#line 20 "cssproperties.gperf"
      {"border-top-left-radius", 14},
#line 92 "cssproperties.gperf"
      {"page-break-after", 86},
#line 54 "cssproperties.gperf"
      {"font-variant", 48},
#line 91 "cssproperties.gperf"
      {"-khtml-padding-start", 85},
#line 12 "cssproperties.gperf"
      {"background-position-x", 6},
#line 121 "cssproperties.gperf"
      {"border-style", 115},
#line 59 "cssproperties.gperf"
      {"line-height", 53},
#line 186 "cssproperties.gperf"
      {"flex-direction", 180},
#line 30 "cssproperties.gperf"
      {"border-top-style", 24},
#line 79 "cssproperties.gperf"
      {"opacity", 73},
#line 106 "cssproperties.gperf"
      {"text-transform", 100},
#line 19 "cssproperties.gperf"
      {"border-bottom-left-radius", 13},
#line 48 "cssproperties.gperf"
      {"display", 42},
#line 67 "cssproperties.gperf"
      {"-khtml-margin-start", 61},
#line 49 "cssproperties.gperf"
      {"empty-cells", 43},
#line 76 "cssproperties.gperf"
      {"min-height", 70},
#line 159 "cssproperties.gperf"
      {"font-size-adjust", 153},
#line 148 "cssproperties.gperf"
      {"color-interpolation-filters", 142},
#line 174 "cssproperties.gperf"
      {"stop-opacity", 168},
#line 129 "cssproperties.gperf"
      {"list-style", 123},
#line 32 "cssproperties.gperf"
      {"border-bottom-style", 26},
#line 160 "cssproperties.gperf"
      {"font-stretch", 154},
#line 10 "cssproperties.gperf"
      {"background-attachment", 4},
#line 68 "cssproperties.gperf"
      {"-khtml-marquee", 62},
#line 74 "cssproperties.gperf"
      {"max-height", 68},
#line 140 "cssproperties.gperf"
      {"scrollbar-arrow-color", 134},
#line 25 "cssproperties.gperf"
      {"-khtml-border-vertical-spacing", 19},
#line 100 "cssproperties.gperf"
      {"table-layout", 94},
#line 72 "cssproperties.gperf"
      {"-khtml-marquee-speed", 66},
#line 142 "cssproperties.gperf"
      {"-khtml-user-input", 136},
#line 144 "cssproperties.gperf"
      {"baseline-shift", 138},
#line 116 "cssproperties.gperf"
      {"word-spacing", 110},
#line 82 "cssproperties.gperf"
      {"outline-style", 76},
#line 60 "cssproperties.gperf"
      {"list-style-image", 54},
#line 181 "cssproperties.gperf"
      {"stroke-opacity", 175},
#line 111 "cssproperties.gperf"
      {"visibility", 105},
#line 61 "cssproperties.gperf"
      {"list-style-position", 55},
#line 185 "cssproperties.gperf"
      {"writing-mode", 179},
#line 69 "cssproperties.gperf"
      {"-khtml-marquee-direction", 63},
#line 71 "cssproperties.gperf"
      {"-khtml-marquee-repetition", 65},
#line 70 "cssproperties.gperf"
      {"-khtml-marquee-increment", 64},
#line 136 "cssproperties.gperf"
      {"scrollbar-highlight-color", 130},
#line 114 "cssproperties.gperf"
      {"width", 108},
#line 126 "cssproperties.gperf"
      {"border-width", 120},
#line 31 "cssproperties.gperf"
      {"border-right-style", 25},
#line 34 "cssproperties.gperf"
      {"border-top-width", 28},
#line 24 "cssproperties.gperf"
      {"-khtml-border-horizontal-spacing", 18},
#line 112 "cssproperties.gperf"
      {"white-space", 106},
#line 135 "cssproperties.gperf"
      {"scrollbar-shadow-color", 129},
#line 81 "cssproperties.gperf"
      {"outline-offset", 75},
#line 33 "cssproperties.gperf"
      {"border-left-style", 27},
#line 36 "cssproperties.gperf"
      {"border-bottom-width", 30},
#line 158 "cssproperties.gperf"
      {"flood-opacity", 152},
#line 53 "cssproperties.gperf"
      {"font-style", 47},
#line 176 "cssproperties.gperf"
      {"stroke-dasharray", 170},
#line 77 "cssproperties.gperf"
      {"min-width", 71},
#line 105 "cssproperties.gperf"
      {"text-shadow", 99},
#line 154 "cssproperties.gperf"
      {"fill-opacity", 148},
#line 13 "cssproperties.gperf"
      {"background-position-y", 7},
#line 84 "cssproperties.gperf"
      {"overflow", 78},
#line 75 "cssproperties.gperf"
      {"max-width", 69},
#line 182 "cssproperties.gperf"
      {"stroke-width", 176},
#line 138 "cssproperties.gperf"
      {"scrollbar-darkshadow-color", 132},
#line 83 "cssproperties.gperf"
      {"outline-width", 77},
#line 190 "cssproperties.gperf"
      {"flex-wrap", 184},
#line 189 "cssproperties.gperf"
      {"flex-grow", 183},
#line 162 "cssproperties.gperf"
      {"glyph-orientation-vertical", 156},
#line 85 "cssproperties.gperf"
      {"overflow-x", 79},
#line 177 "cssproperties.gperf"
      {"stroke-dashoffset", 171},
#line 104 "cssproperties.gperf"
      {"text-overflow", 98},
#line 187 "cssproperties.gperf"
      {"justify-content", 181},
#line 35 "cssproperties.gperf"
      {"border-right-width", 29},
#line 62 "cssproperties.gperf"
      {"list-style-type", 56},
#line 115 "cssproperties.gperf"
      {"word-wrap", 109},
#line 37 "cssproperties.gperf"
      {"border-left-width", 31},
#line 73 "cssproperties.gperf"
      {"-khtml-marquee-style", 67},
#line 113 "cssproperties.gperf"
      {"widows", 107},
#line 55 "cssproperties.gperf"
      {"font-weight", 49},
#line 161 "cssproperties.gperf"
      {"glyph-orientation-horizontal", 155},
#line 51 "cssproperties.gperf"
      {"font-family", 45},
#line 141 "cssproperties.gperf"
      {"-khtml-flow-mode", 135},
#line 86 "cssproperties.gperf"
      {"overflow-y", 80}
    };
#if (defined __GNUC__ && __GNUC__ + (__GNUC_MINOR__ >= 6) > 4) || (defined __clang__ && __clang_major__ >= 3)
#pragma GCC diagnostic pop
#endif

  static const short lookup[] =
    {
       -1,  -1,  -1,  -1,  -1,   0,   1,  -1,   2,  -1,
        3,  -1,   4,   5,  -1,   6,  -1,  -1,  -1,  -1,
       -1,   7,  -1,   8,   9,  10,  -1,  -1,  -1,  -1,
       11,  -1,  -1,  -1,  -1,  -1,  12,  -1,  -1,  -1,
       -1,  -1,  -1,  13,  14,  -1,  -1,  -1,  -1,  15,
       16,  -1,  -1,  -1,  -1,  -1,  -1,  17,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  18,  19,  -1,  -1,
       -1,  -1,  -1,  -1,  20,  -1,  21,  -1,  -1,  22,
       -1,  -1,  -1,  23,  24,  -1,  25,  -1,  -1,  -1,
       26,  27,  28,  -1,  29,  30,  -1,  -1,  31,  -1,
       -1,  32,  33,  34,  35,  -1,  -1,  -1,  36,  37,
       -1,  -1,  -1,  -1,  38,  -1,  39,  40,  -1,  -1,
       -1,  -1,  -1,  41,  42,  43,  44,  -1,  -1,  -1,
       -1,  -1,  45,  -1,  46,  47,  48,  -1,  -1,  -1,
       49,  -1,  50,  51,  52,  53,  54,  -1,  -1,  -1,
       55,  -1,  56,  -1,  57,  -1,  -1,  58,  -1,  59,
       60,  -1,  61,  -1,  -1,  62,  -1,  63,  -1,  -1,
       64,  -1,  -1,  65,  -1,  -1,  66,  67,  68,  -1,
       -1,  -1,  69,  -1,  -1,  70,  -1,  71,  -1,  -1,
       -1,  -1,  -1,  72,  73,  74,  75,  -1,  -1,  -1,
       76,  77,  78,  -1,  -1,  -1,  -1,  79,  -1,  80,
       -1,  -1,  81,  -1,  82,  83,  84,  -1,  85,  -1,
       86,  87,  -1,  -1,  88,  89,  -1,  -1,  90,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  91,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  92,  -1,  93,
       -1,  -1,  94,  -1,  95,  -1,  -1,  -1,  -1,  96,
       -1,  -1,  97,  -1,  98,  -1,  -1,  -1,  99,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 100,  -1,  -1,  -1,
       -1, 101,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 102, 103,  -1,  -1,
       -1,  -1, 104,  -1,  -1,  -1, 105, 106,  -1,  -1,
       -1,  -1,  -1,  -1,  -1, 107, 108, 109,  -1,  -1,
       -1, 110,  -1,  -1, 111,  -1, 112, 113,  -1, 114,
      115,  -1, 116,  -1, 117,  -1, 118,  -1,  -1,  -1,
      119, 120, 121,  -1,  -1,  -1,  -1, 122,  -1,  -1,
      123,  -1,  -1,  -1, 124,  -1,  -1, 125,  -1,  -1,
       -1, 126,  -1,  -1, 127, 128, 129,  -1,  -1,  -1,
      130,  -1, 131,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
      132,  -1, 133,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1, 134,  -1,  -1, 135,  -1,  -1,
       -1,  -1,  -1, 136,  -1,  -1, 137,  -1,  -1, 138,
       -1,  -1,  -1,  -1,  -1, 139,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1, 140,  -1,  -1, 141,  -1, 142,
       -1,  -1,  -1,  -1,  -1, 143,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1, 144,
      145,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
      146,  -1,  -1,  -1,  -1,  -1,  -1, 147, 148,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 149, 150,  -1,  -1,
       -1, 151, 152,  -1, 153,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1, 154,  -1, 155,  -1,  -1,  -1, 156,  -1,
       -1,  -1,  -1,  -1,  -1, 157,  -1,  -1,  -1,  -1,
       -1, 158,  -1,  -1, 159,  -1, 160, 161,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 162,  -1, 163,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1, 164,
       -1,  -1, 165,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1, 166,  -1, 167,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1, 168,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1, 169,  -1,  -1,  -1,  -1,  -1,
       -1, 170,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
      171,  -1,  -1,  -1,  -1,  -1,  -1, 172,  -1,  -1,
       -1,  -1,  -1, 173,  -1,  -1,  -1,  -1,  -1,  -1,
      174,  -1,  -1,  -1,  -1,  -1,  -1,  -1, 175,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
      176,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1, 177,  -1,  -1,  -1,  -1,  -1,
       -1,  -1, 178,  -1,  -1, 179,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 180,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1, 181,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1, 182,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1, 183,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1, 184,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
       -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
      185
    };

  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      register unsigned int key = hash_prop (str, len);

      if (key <= MAX_HASH_VALUE)
        {
          register int index = lookup[key];

          if (index >= 0)
            {
              register const char *s = wordlist_prop[index].name;

              if (*str == *s && !strncmp (str + 1, s + 1, len - 1) && s[len] == '\0')
                return &wordlist_prop[index];
            }
        }
    }
  return (struct css_prop *) 0;
}
#line 193 "cssproperties.gperf"


static const char * const propertyList[] = {
    nullptr,
    "background-color",
    "background-image",
    "background-repeat",
    "background-attachment",
    "background-position",
    "background-position-x",
    "background-position-y",
    "background-clip",
    "background-origin",
    "background-size",
    "border-top-right-radius",
    "border-bottom-right-radius",
    "border-bottom-left-radius",
    "border-top-left-radius",
    "border-radius",
    "border-collapse",
    "border-spacing",
    "-khtml-border-horizontal-spacing",
    "-khtml-border-vertical-spacing",
    "border-top-color",
    "border-right-color",
    "border-bottom-color",
    "border-left-color",
    "border-top-style",
    "border-right-style",
    "border-bottom-style",
    "border-left-style",
    "border-top-width",
    "border-right-width",
    "border-bottom-width",
    "border-left-width",
    "bottom",
    "caption-side",
    "clear",
    "clip",
    "color",
    "content",
    "counter-increment",
    "counter-reset",
    "cursor",
    "direction",
    "display",
    "empty-cells",
    "float",
    "font-family",
    "font-size",
    "font-style",
    "font-variant",
    "font-weight",
    "height",
    "left",
    "letter-spacing",
    "line-height",
    "list-style-image",
    "list-style-position",
    "list-style-type",
    "margin-top",
    "margin-right",
    "margin-bottom",
    "margin-left",
    "-khtml-margin-start",
    "-khtml-marquee",
    "-khtml-marquee-direction",
    "-khtml-marquee-increment",
    "-khtml-marquee-repetition",
    "-khtml-marquee-speed",
    "-khtml-marquee-style",
    "max-height",
    "max-width",
    "min-height",
    "min-width",
    "orphans",
    "opacity",
    "outline-color",
    "outline-offset",
    "outline-style",
    "outline-width",
    "overflow",
    "overflow-x",
    "overflow-y",
    "padding-top",
    "padding-right",
    "padding-bottom",
    "padding-left",
    "-khtml-padding-start",
    "page-break-after",
    "page-break-before",
    "page-break-inside",
    "position",
    "quotes",
    "right",
    "size",
    "src",
    "table-layout",
    "text-align",
    "text-decoration",
    "text-indent",
    "text-overflow",
    "text-shadow",
    "text-transform",
    "top",
    "unicode-bidi",
    "unicode-range",
    "vertical-align",
    "visibility",
    "white-space",
    "widows",
    "width",
    "word-wrap",
    "word-spacing",
    "z-index",
    "background",
    "border",
    "border-color",
    "border-style",
    "border-top",
    "border-right",
    "border-bottom",
    "border-left",
    "border-width",
    "box-sizing",
    "font",
    "list-style",
    "margin",
    "outline",
    "padding",
    "scrollbar-base-color",
    "scrollbar-face-color",
    "scrollbar-shadow-color",
    "scrollbar-highlight-color",
    "scrollbar-3dlight-color",
    "scrollbar-darkshadow-color",
    "scrollbar-track-color",
    "scrollbar-arrow-color",
    "-khtml-flow-mode",
    "-khtml-user-input",
    "alignment-baseline",
    "baseline-shift",
    "clip-path",
    "clip-rule",
    "color-interpolation",
    "color-interpolation-filters",
    "color-profile",
    "color-rendering",
    "dominant-baseline",
    "enable-background",
    "fill",
    "fill-opacity",
    "fill-rule",
    "filter",
    "flood-color",
    "flood-opacity",
    "font-size-adjust",
    "font-stretch",
    "glyph-orientation-horizontal",
    "glyph-orientation-vertical",
    "image-rendering",
    "kerning",
    "lighting-color",
    "marker",
    "marker-end",
    "marker-mid",
    "marker-start",
    "mask",
    "pointer-events",
    "shape-rendering",
    "stop-color",
    "stop-opacity",
    "stroke",
    "stroke-dasharray",
    "stroke-dashoffset",
    "stroke-linecap",
    "stroke-linejoin",
    "stroke-miterlimit",
    "stroke-opacity",
    "stroke-width",
    "text-anchor",
    "text-rendering",
    "writing-mode",
    "flex-direction",
    "justify-content",
    "align-items",
    "flex-grow",
    "flex-wrap",
    "gap",
    "flex",
    nullptr
};
DOMString getPropertyName(unsigned short id)
{
    if(id >= CSS_PROP_TOTAL || id == 0)
      return DOMString();
    else
      return DOMString(propertyList[id]);
}
