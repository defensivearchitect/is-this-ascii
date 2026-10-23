/*
 * is-this-ascii.c
 *
 * Bir metnin yalnizca ASCII karakterlerden olustugunu dogrular. Sorun yoksa
 * yesil "OK" yazar; ASCII olmayan her karakteri bulundugu satirla birlikte
 * kirmizi gosterir.
 *
 * Internete baglanmaz: ag kodu icermez. Yalnizca panoyu (clipboard),
 * konsolu ve verdiginiz dosyalari okur; hicbir seyi degistirmez.
 *
 * Kullanim:
 *   is-this-ascii           Panodaki metni kontrol eder (Windows). Metni
 *                           kopyalayin (Ctrl+C), programda Enter'a basin.
 *                           Cikmak icin q yazip Enter'a basin.
 *   is-this-ascii DOSYA...  Dosyalari kontrol eder.
 *
 * Kabul edilenler: yazdirilabilir ASCII (0x20-0x7E, bosluk dahil), TAB ve
 * satir sonu (LF ya da CRLF). Geri kalan her sey sorun sayilir: kivrik
 * tirnaklar, uzun tireler, gorunmez karakterler, Turkce harfler, kontrol
 * karakterleri, gecersiz baytlar.
 *
 * Cikis kodu: 0 = hepsi ASCII, 1 = sorun bulundu, 2 = okunamadi.
 *
 * Derleme (Windows, MinGW GCC):
 *   gcc -O2 -Wall -Wextra -o is-this-ascii.exe is-this-ascii.c
 *
 * Neden metni yapistirmak yerine pano okunuyor: Windows konsolu yapistirilan
 * metni programa ulasmadan degistirebilir. Konsolun "FilterOnPaste" ayari
 * ornegin kivrik tirnaklari duz tirnaga cevirir; konsolun kod sayfasinda
 * olmayan karakterler de '?' olabilir. Program o zaman duzeltilmis metne
 * bakip yanlislikla OK derdi. Pano dogrudan okununca, baska yere
 * yapistiracaginiz metnin kendisi kontrol edilir.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#endif

/* Renk kodlari; yalnizca cikti bir konsola gidiyorsa yazilir. */
#define RED "\x1b[1;31m"
#define GREEN "\x1b[1;32m"
#define RESET "\x1b[0m"

/* Gecersiz baytlar BAD_BYTE + bayt degeri olarak saklanir. Bu deger Unicode
 * araliginin (en fazla U+10FFFF) disinda oldugu icin hicbir karakterle
 * karismaz. */
#define BAD_BYTE 0x110000u

#define MAX_LINES 50   /* raporda en fazla bu kadar sorunlu satir gosterilir */
#define MAX_COLS 10    /* bir satir icin en fazla bu kadar sutun numarasi */
#define MAX_KINDS 40   /* ozette en fazla bu kadar farkli karakter listelenir */
#define LONG_LINE 1000 /* daha uzun satirlarin yalnizca bir parcasi gosterilir */
#define PREVIEW 50     /* onizlemede gosterilen karakter sayisi */

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int use_color;

static void color(const char *code)
{
    if (use_color)
        fputs(code, stdout);
}

/* ---- Hangi karakterler kabul ediliyor -------------------------------- */

/* t[i] kabul edilen bir karakter mi: yazdirilabilir ASCII, TAB, LF ya da
 * CRLF'nin CR'si. Tek basina CR kabul edilmez: terminalde satirin basina
 * donup yazinin ustune yazdirir, metnin bir kismini gizleyebilir. */
static int is_allowed(const uint32_t *t, size_t n, size_t i)
{
    uint32_t c = t[i];

    if (c >= 0x20 && c <= 0x7E)
        return 1;
    if (c == '\t' || c == '\n')
        return 1;
    return c == '\r' && i + 1 < n && t[i + 1] == '\n';
}

/* ---- Metni karakterlere (kod noktalarina) cevirme -------------------- */

/* s'nin basindaki UTF-8 dizisini cozer. Gecerliyse uzunlugunu (1-4)
 * dondurur ve karakteri *cp'ye yazar; gecersizse 0 dondurur. Gereksiz uzun
 * kodlamalar, UTF-16 yarimlari ve U+10FFFF ustu gecersiz sayilir. */
static size_t utf8_seq(const unsigned char *s, size_t avail, uint32_t *cp)
{
    uint32_t c;
    size_t len, i;

    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        len = 2;
        c = s[0] & 0x1Fu;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        len = 3;
        c = s[0] & 0x0Fu;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        len = 4;
        c = s[0] & 0x07u;
    } else {
        return 0;
    }
    if (len > avail)
        return 0;
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF ||
        (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return len;
}

/* UTF-8 baytlarini karakterlere cevirir. Gecersiz her bayt ayri bir sorun
 * olarak kalir; hicbir bayt atlanmaz. */
static size_t decode_utf8(const unsigned char *s, size_t len, uint32_t *out)
{
    size_t i = 0, n = 0, k;
    uint32_t c;

    while (i < len) {
        k = utf8_seq(s + i, len - i, &c);
        if (k == 0) {
            out[n++] = BAD_BYTE + (uint32_t)s[i];
            i++;
        } else {
            out[n++] = c;
            i += k;
        }
    }
    return n;
}

/* UTF-16'yi (Windows panosunun bicimi) karakterlere cevirir. Eslesmeyen
 * yarim (D800-DFFF) oldugu gibi kalir ve sorun olarak gosterilir. */
static size_t decode_utf16(const uint16_t *s, size_t len, uint32_t *out)
{
    size_t i = 0, n = 0;
    uint32_t c;

    while (i < len) {
        c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && i < len && s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)s[i] - 0xDC00);
            i++;
        }
        out[n++] = c;
    }
    return n;
}

/* ---- Karakter aciklamalari ------------------------------------------- */

struct known {
    uint32_t c;
    const char *desc;
    const char *fix; /* ASCII karsiligi; NULL = oneri yok, "" = silinmeli */
};

static const struct known known_chars[] = {
    /* Kontrol karakterleri */
    {0x0000, "NUL (bos karakter)", ""},
    {0x0007, "BEL (zil)", ""},
    {0x0008, "BS (geri silme)", ""},
    {0x000B, "VT (dikey sekme)", ""},
    {0x000C, "FF (sayfa sonu)", ""},
    {0x000D, "tek basina CR (satir basi)", ""},
    {0x001B, "ESC (terminal kacis dizisi baslatir)", ""},
    {0x007F, "DEL (silme)", ""},
    {0x0085, "NEL (Unicode satir sonu)", NULL},

    /* Bosluklar ve gorunmez karakterler */
    {0x00A0, "bolunmez bosluk (NBSP)", " "},
    {0x00AD, "gorunmez yumusak tire (SHY)", ""},
    {0x061C, "gorunmez yon isareti (ALM)", ""},
    {0x115F, "bos gorunen Hangul dolgu karakteri", ""},
    {0x1160, "bos gorunen Hangul dolgu karakteri", ""},
    {0x180E, "gorunmez Mogol unlu ayirici", ""},
    {0x200B, "sifir genislikli bosluk (ZWSP, gorunmez)", ""},
    {0x200C, "sifir genislikli ayirici (ZWNJ, gorunmez)", ""},
    {0x200D, "sifir genislikli birlestirici (ZWJ, gorunmez)", ""},
    {0x200E, "gorunmez yon isareti (LRM)", ""},
    {0x200F, "gorunmez yon isareti (RLM)", ""},
    {0x2028, "satir ayirici (LS, gorunmez)", NULL},
    {0x2029, "paragraf ayirici (PS, gorunmez)", NULL},
    {0x202A, "gorunmez yon karakteri LRE (Trojan Source)", ""},
    {0x202B, "gorunmez yon karakteri RLE (Trojan Source)", ""},
    {0x202C, "gorunmez yon karakteri PDF (Trojan Source)", ""},
    {0x202D, "gorunmez yon karakteri LRO (Trojan Source)", ""},
    {0x202E, "RLO: metni ters gosterir (Trojan Source)", ""},
    {0x202F, "dar bolunmez bosluk", " "},
    {0x205F, "matematik boslugu", " "},
    {0x2060, "kelime birlestirici (WJ, gorunmez)", ""},
    {0x2066, "gorunmez yon karakteri LRI (Trojan Source)", ""},
    {0x2067, "gorunmez yon karakteri RLI (Trojan Source)", ""},
    {0x2068, "gorunmez yon karakteri FSI (Trojan Source)", ""},
    {0x2069, "gorunmez yon karakteri PDI (Trojan Source)", ""},
    {0x3000, "genis bosluk (ideografik)", " "},
    {0x3164, "bos gorunen Hangul dolgu karakteri", ""},
    {0xFEFF, "BOM / sifir genislikli bosluk (gorunmez)", ""},
    {0xFFA0, "bos gorunen Hangul dolgu karakteri", ""},
    {0xFFFD, "bozuk kodlama isareti (yedek karakter)", NULL},

    /* Tirnaklar ve kesme isaretleri */
    {0x00AB, "acili tirnak", "\""},
    {0x00B4, "aksan isareti", "'"},
    {0x00BB, "acili tirnak", "\""},
    {0x02BC, "kesme isareti harfi", "'"},
    {0x2018, "sol kivrik tek tirnak", "'"},
    {0x2019, "sag kivrik tek tirnak (kesme)", "'"},
    {0x201A, "alt tek tirnak", "'"},
    {0x201B, "ters tek tirnak", "'"},
    {0x201C, "sol kivrik cift tirnak", "\""},
    {0x201D, "sag kivrik cift tirnak", "\""},
    {0x201E, "alt cift tirnak", "\""},
    {0x201F, "ters cift tirnak", "\""},
    {0x2032, "prime (dakika) isareti", "'"},
    {0x2033, "cift prime (saniye) isareti", "\""},
    {0x2039, "acili tek tirnak", "'"},
    {0x203A, "acili tek tirnak", "'"},

    /* Tireler */
    {0x2010, "Unicode kisa cizgi", "-"},
    {0x2011, "bolunmez kisa cizgi", "-"},
    {0x2012, "rakam tiresi", "-"},
    {0x2013, "orta tire (en dash)", "-"},
    {0x2014, "uzun tire (em dash)", "-"},
    {0x2015, "yatay cizgi", "-"},
    {0x2212, "matematik eksi isareti", "-"},

    /* Diger isaretler */
    {0x00A9, "telif isareti", "(c)"},
    {0x00AE, "tescilli marka isareti", "(R)"},
    {0x00B0, "derece isareti", NULL},
    {0x00B7, "orta nokta", NULL},
    {0x00D7, "carpi isareti", "x"},
    {0x00F7, "bolu isareti", "/"},
    {0x2022, "madde isareti (bullet)", "-"},
    {0x2026, "uc nokta (tek karakter)", "..."},
    {0x2044, "kesir cizgisi", "/"},
    {0x20AC, "euro isareti", "EUR"},
    {0x20BA, "Turk lirasi isareti", "TL"},
    {0x2122, "ticari marka isareti", "(TM)"},
    {0x2190, "sol ok", "<-"},
    {0x2192, "sag ok", "->"},
    {0x21D2, "cift cizgili sag ok", "=>"},
    {0x2215, "bolme cizgisi", "/"},
    {0x2217, "yildiz isleci", "*"},
    {0x2260, "esit degil isareti", "!="},
    {0x2264, "kucuk esit isareti", "<="},
    {0x2265, "buyuk esit isareti", ">="},

    /* Turkce harfler */
    {0x00C7, "Turkce buyuk C harfi (cengelli)", "C"},
    {0x00D6, "Turkce buyuk O harfi (iki noktali)", "O"},
    {0x00DC, "Turkce buyuk U harfi (iki noktali)", "U"},
    {0x00E7, "Turkce kucuk c harfi (cengelli)", "c"},
    {0x00F6, "Turkce kucuk o harfi (iki noktali)", "o"},
    {0x00FC, "Turkce kucuk u harfi (iki noktali)", "u"},
    {0x011E, "Turkce buyuk yumusak G", "G"},
    {0x011F, "Turkce kucuk yumusak g", "g"},
    {0x0130, "Turkce noktali buyuk I (Shift+i)", "I"},
    {0x0131, "Turkce noktasiz kucuk i", "i"},
    {0x015E, "Turkce buyuk S harfi (cengelli)", "S"},
    {0x015F, "Turkce kucuk s harfi (cengelli)", "s"},
};

struct span {
    uint32_t lo, hi;
    const char *desc;
    const char *fix;
};

/* Tek tek listelenmeyen karakterler icin bloklar; ilk eslesen kullanilir. */
static const struct span spans[] = {
    {0x0001, 0x001F, "kontrol karakteri (gorunmez)", ""},
    {0x0080, 0x009F, "C1 kontrol karakteri (gorunmez)", ""},
    {0x00A0, 0x00BF, "Latin-1 isareti", NULL},
    {0x00C0, 0x024F, "aksanli Latin harfi", NULL},
    {0x0250, 0x02FF, "fonetik harf ya da isaret", NULL},
    {0x0300, 0x036F, "birlesen aksan (onceki harfin ustune biner)", ""},
    {0x0370, 0x03FF, "Yunan harfi", NULL},
    {0x0400, 0x052F, "Kiril harfi", NULL},
    {0x0530, 0x058F, "Ermeni harfi", NULL},
    {0x0590, 0x05FF, "Ibranice karakter", NULL},
    {0x0600, 0x06FF, "Arapca karakter", NULL},
    {0x2000, 0x200A, "ozel genislikte bosluk", " "},
    {0x2061, 0x2064, "gorunmez matematik isleci", ""},
    {0x206A, 0x206F, "gorunmez bicim karakteri", ""},
    {0x2000, 0x206F, "noktalama isareti", NULL},
    {0x2070, 0x209F, "ust ya da alt simge", NULL},
    {0x20A0, 0x20CF, "para birimi isareti", NULL},
    {0x20D0, 0x20FF, "birlesen isaret (onceki karakterin ustune biner)", ""},
    {0x2100, 0x214F, "harf benzeri sembol", NULL},
    {0x2150, 0x218F, "kesir ya da Roma rakami", NULL},
    {0x2190, 0x21FF, "ok isareti", NULL},
    {0x2200, 0x22FF, "matematik isareti", NULL},
    {0x2300, 0x23FF, "teknik sembol", NULL},
    {0x2460, 0x24FF, "daire icinde harf ya da rakam", NULL},
    {0x2500, 0x257F, "kutu cizim karakteri", NULL},
    {0x2580, 0x25FF, "blok ya da geometrik sekil", NULL},
    {0x2600, 0x27BF, "sembol (onay, yildiz vb.)", NULL},
    {0x2E80, 0x9FFF, "Dogu Asya karakteri", NULL},
    {0xAC00, 0xD7AF, "Korece hece", NULL},
    {0xD800, 0xDFFF, "eslesmemis UTF-16 yarimi (bozuk karakter)", NULL},
    {0xE000, 0xF8FF, "ozel kullanim alani karakteri", NULL},
    {0xFE00, 0xFE0F, "gorunmez varyasyon secici", ""},
    {0xFE20, 0xFE2F, "birlesen yarim isaret", ""},
    {0xFF00, 0xFFEF, "tam ya da yarim genislikli karakter", NULL},
    {0x1D400, 0x1D7FF, "matematiksel harf (normal harf gibi gorunur)", NULL},
    {0x1F000, 0x1FAFF, "emoji ya da sembol", NULL},
    {0xE0000, 0xE007F, "gorunmez etiket karakteri", ""},
    {0xE0100, 0xE01EF, "gorunmez varyasyon secici", ""},
    {0xF0000, 0x10FFFF, "ozel kullanim alani karakteri", NULL},
};

struct lookalike {
    uint32_t c;
    char latin;
};

/* Latin harflerinin aynisi gibi gorunen harfler. Sahte alan adlari ve
 * kullanici adlari (homoglif saldirilari) bunlarla yapilir. */
static const struct lookalike lookalikes[] = {
    /* Yunan */
    {0x0391, 'A'}, {0x0392, 'B'}, {0x0395, 'E'}, {0x0396, 'Z'}, {0x0397, 'H'},
    {0x0399, 'I'}, {0x039A, 'K'}, {0x039C, 'M'}, {0x039D, 'N'}, {0x039F, 'O'},
    {0x03A1, 'P'}, {0x03A4, 'T'}, {0x03A5, 'Y'}, {0x03A7, 'X'}, {0x03BD, 'v'},
    {0x03BF, 'o'}, {0x03C1, 'p'},
    /* Kiril */
    {0x0405, 'S'}, {0x0406, 'I'}, {0x0408, 'J'}, {0x0410, 'A'}, {0x0412, 'B'},
    {0x0415, 'E'}, {0x041A, 'K'}, {0x041C, 'M'}, {0x041D, 'H'}, {0x041E, 'O'},
    {0x0420, 'P'}, {0x0421, 'C'}, {0x0422, 'T'}, {0x0423, 'Y'}, {0x0425, 'X'},
    {0x0430, 'a'}, {0x0435, 'e'}, {0x043E, 'o'}, {0x0440, 'p'}, {0x0441, 'c'},
    {0x0443, 'y'}, {0x0445, 'x'}, {0x0455, 's'}, {0x0456, 'i'}, {0x0458, 'j'},
    {0x04AE, 'Y'}, {0x04BB, 'h'}, {0x04C0, 'I'}, {0x04CF, 'l'}, {0x0501, 'd'},
    {0x051B, 'q'}, {0x051D, 'w'},
    /* Ermeni */
    {0x0555, 'O'}, {0x0585, 'o'},
};

static const struct known *find_known(uint32_t c)
{
    size_t i;

    for (i = 0; i < COUNT(known_chars); i++)
        if (known_chars[i].c == c)
            return &known_chars[i];
    return NULL;
}

static const struct span *find_span(uint32_t c)
{
    size_t i;

    for (i = 0; i < COUNT(spans); i++)
        if (c >= spans[i].lo && c <= spans[i].hi)
            return &spans[i];
    return NULL;
}

static char find_lookalike(uint32_t c)
{
    size_t i;

    for (i = 0; i < COUNT(lookalikes); i++)
        if (lookalikes[i].c == c)
            return lookalikes[i].latin;
    return '\0';
}

/* c icin Turkce bir aciklama (desc) ve duzeltme onerisi (hint) yazar. */
static void describe(uint32_t c, char *desc, size_t dsize, char *hint, size_t hsize)
{
    const struct known *k = find_known(c);
    const struct span *s;
    const char *fix = NULL;
    char latin[2] = {0, 0};

    if (c >= BAD_BYTE) {
        snprintf(desc, dsize, "UTF-8 olmayan bayt (dosya baska kodlamada olabilir)");
    } else if (k != NULL) {
        snprintf(desc, dsize, "%s", k->desc);
        fix = k->fix;
    } else if (c >= 0xFF01 && c <= 0xFF5E) {
        /* Tam genislikli ASCII, 0x21-0x7E ile ayni sirada durur. */
        latin[0] = (char)(c - 0xFEE0);
        snprintf(desc, dsize, "tam genislikli (fullwidth) karakter");
        fix = latin;
    } else if (c >= 0xE0020 && c <= 0xE007E) {
        /* Etiket karakterleri gorunmez ama her biri bir ASCII karakteri
         * tasir; metne gizli yazi saklamak icin kullanilir. */
        snprintf(desc, dsize, "gorunmez etiket karakteri, gizli '%c' tasiyor",
                 (int)(c - 0xE0000));
        fix = "";
    } else {
        s = find_span(c);
        latin[0] = find_lookalike(c);
        if (latin[0] != '\0') {
            snprintf(desc, dsize, "%s; Latin %c harfine benzer", s != NULL ? s->desc : "harf",
                     latin[0]);
            fix = latin;
        } else if (s != NULL) {
            snprintf(desc, dsize, "%s", s->desc);
            fix = s->fix;
        } else {
            snprintf(desc, dsize, "ASCII disi karakter");
        }
    }

    if (fix == NULL)
        snprintf(hint, hsize, "%s", "");
    else if (fix[0] == '\0')
        snprintf(hint, hsize, "silin");
    else if (strcmp(fix, " ") == 0)
        snprintf(hint, hsize, "yerine: bosluk");
    else
        snprintf(hint, hsize, "yerine: %s", fix);
}

/* ---- Rapor ----------------------------------------------------------- */

/* Karakter kodunu yazar: "U+201C" ya da gecersiz bayt icin "0xE9". */
static void code_str(uint32_t c, char *buf, size_t size)
{
    if (c >= BAD_BYTE)
        snprintf(buf, size, "0x%02lX", (unsigned long)(c - BAD_BYTE));
    else
        snprintf(buf, size, "U+%04lX", (unsigned long)c);
}

/* t[from, to) araligini yazar. Kabul edilmeyen karakterler kirmizi [U+XXXX]
 * olarak yazilir; metindeki kontrol karakterleri asla oldugu gibi
 * yazdirilmaz, boylece kontrol edilen metin terminali etkileyemez. */
static void print_span(const uint32_t *t, size_t n, size_t from, size_t to)
{
    char code[16];
    size_t i;

    for (i = from; i < to; i++) {
        if (is_allowed(t, n, i)) {
            if (t[i] != '\r' && t[i] != '\n')
                putchar((int)t[i]);
        } else {
            code_str(t[i], code, sizeof code);
            color(RED);
            printf("[%s]", code);
            color(RESET);
        }
    }
}

static unsigned long count_lines(const uint32_t *t, size_t n)
{
    unsigned long lines = 0;
    size_t i;

    for (i = 0; i < n; i++)
        if (t[i] == '\n')
            lines++;
    if (n > 0 && t[n - 1] != '\n')
        lines++;
    return lines;
}

/* Bulunan farkli karakterler ve kacar kez gectikleri */
struct summary {
    uint32_t c[MAX_KINDS];
    unsigned long count[MAX_KINDS];
    size_t used;
    unsigned long unlisted; /* liste dolduktan sonra gelenler */
};

static void add_kind(struct summary *sum, uint32_t c)
{
    size_t i;

    for (i = 0; i < sum->used; i++) {
        if (sum->c[i] == c) {
            sum->count[i]++;
            return;
        }
    }
    if (sum->used == MAX_KINDS) {
        sum->unlisted++;
        return;
    }
    sum->c[sum->used] = c;
    sum->count[sum->used] = 1;
    sum->used++;
}

/* Sorunlu bir satiri yazar: satir ve sutun numaralari, sonra satirin
 * kendisi. first, satirdaki ilk sorunlu karakterin yeridir. */
static void show_line(const uint32_t *t, size_t n, size_t start, size_t end, size_t first,
                      unsigned long line_no)
{
    size_t i, from = start, to = end;
    unsigned long cols = 0;

    printf("\nSatir %lu, sutun ", line_no);
    for (i = start; i < end; i++) {
        if (is_allowed(t, n, i))
            continue;
        if (cols == MAX_COLS) {
            fputs(", ...", stdout);
            break;
        }
        if (cols > 0)
            fputs(", ", stdout);
        printf("%lu", (unsigned long)(i - start + 1));
        cols++;
    }
    puts(":");

    /* Cok uzun satirlarda ilk sorunun cevresi gosterilir. */
    if (end - start > LONG_LINE) {
        from = first - start > 40 ? first - 40 : start;
        to = end - from > LONG_LINE ? from + LONG_LINE : end;
    }
    if (from > start)
        fputs("...", stdout);
    print_span(t, n, from, to);
    if (to < end)
        fputs("...", stdout);
    putchar('\n');
}

static void show_summary(const struct summary *sum)
{
    char code[16], desc[96], hint[32];
    size_t i, len, width = 0;

    /* Aciklamalari alt alta hizalamak icin once en uzununu bul. */
    for (i = 0; i < sum->used; i++) {
        describe(sum->c[i], desc, sizeof desc, hint, sizeof hint);
        len = strlen(desc);
        if (len > width)
            width = len;
    }

    puts("\nBulunan karakterler:");
    for (i = 0; i < sum->used; i++) {
        code_str(sum->c[i], code, sizeof code);
        describe(sum->c[i], desc, sizeof desc, hint, sizeof hint);
        fputs("  ", stdout);
        color(RED);
        printf("%-8s", code);
        color(RESET);
        printf("%5lu kez   ", sum->count[i]);
        if (hint[0] != '\0')
            printf("%-*s   %s\n", (int)width, desc, hint);
        else
            printf("%s\n", desc);
    }
    if (sum->unlisted > 0)
        printf("  ... ve listeye sigmayan %lu karakter daha\n", sum->unlisted);
}

/* Metni satir satir kontrol eder ve raporu yazar: sorunlu satirlar
 * (sorunlu karakterler kirmizi), bulunan karakterlerin ozeti ve sonuc.
 * ASCII olmayan karakter sayisini dondurur. */
static unsigned long report(const uint32_t *t, size_t n)
{
    struct summary sum;
    size_t start = 0, end, i, first;
    unsigned long line_no = 1, bad = 0, bad_lines = 0, in_line;

    memset(&sum, 0, sizeof sum);
    for (;;) {
        end = start;
        while (end < n && t[end] != '\n')
            end++;

        in_line = 0;
        first = end;
        for (i = start; i < end; i++) {
            if (!is_allowed(t, n, i)) {
                if (in_line == 0)
                    first = i;
                in_line++;
                add_kind(&sum, t[i]);
            }
        }
        if (in_line > 0) {
            bad += in_line;
            bad_lines++;
            if (bad_lines <= MAX_LINES)
                show_line(t, n, start, end, first, line_no);
        }

        if (end == n)
            break;
        start = end + 1;
        line_no++;
    }

    if (bad == 0) {
        color(GREEN);
        fputs("OK - tum karakterler ASCII", stdout);
    } else {
        if (bad_lines > MAX_LINES)
            printf("\n... ve %lu sorunlu satir daha\n", bad_lines - MAX_LINES);
        show_summary(&sum);
        putchar('\n');
        color(RED);
        printf("SORUN - %lu karakter ASCII degil (%lu satirda)", bad, bad_lines);
    }
    color(RESET);
    putchar('\n');
    fflush(stdout);
    return bad;
}

/* ---- Dosyalar -------------------------------------------------------- */

/* Dosyanin tamamini bellege okur. Basarisizsa NULL dondurur. */
static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL, *grown;
    size_t cap = 0, got;

    *len = 0;
    if (f == NULL)
        return NULL;
    for (;;) {
        if (*len == cap) {
            cap = cap == 0 ? 65536 : cap * 2;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            buf = grown;
        }
        got = fread(buf + *len, 1, cap - *len, f);
        *len += got;
        if (got == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        errno = EIO;
        return NULL;
    }
    fclose(f);
    return buf;
}

/* Bir dosyayi kontrol eder. 0: hepsi ASCII, 1: sorun var, 2: okunamadi.
 * Dosya UTF-16 isaretiyle (BOM) basliyorsa UTF-16, degilse UTF-8 okunur. */
static int check_file(const char *path)
{
    unsigned char *data;
    uint16_t *units;
    uint32_t *chars;
    size_t len, n, i;
    unsigned long bad;
    int utf16, big_endian;

    data = read_file(path, &len);
    if (data == NULL) {
        fflush(stdout);
        fprintf(stderr, "%s: okunamadi (%s)\n", path, strerror(errno));
        return 2;
    }
    chars = len < SIZE_MAX / sizeof *chars ? malloc((len + 1) * sizeof *chars) : NULL;
    if (chars == NULL) {
        free(data);
        fflush(stdout);
        fprintf(stderr, "%s: bellek yetersiz\n", path);
        return 2;
    }

    big_endian = len >= 2 && data[0] == 0xFE && data[1] == 0xFF;
    utf16 = big_endian || (len >= 2 && data[0] == 0xFF && data[1] == 0xFE);
    if (utf16) {
        units = malloc((len / 2 + 1) * sizeof *units);
        if (units == NULL) {
            free(chars);
            free(data);
            fflush(stdout);
            fprintf(stderr, "%s: bellek yetersiz\n", path);
            return 2;
        }
        for (i = 0; i < len / 2; i++) {
            unsigned hi = big_endian ? data[2 * i] : data[2 * i + 1];
            unsigned lo = big_endian ? data[2 * i + 1] : data[2 * i];
            units[i] = (uint16_t)(hi << 8 | lo);
        }
        n = decode_utf16(units, len / 2, chars);
        if (len % 2 != 0) /* UTF-16'da tek kalan bayt */
            chars[n++] = BAD_BYTE + (uint32_t)data[len - 1];
        free(units);
    } else {
        n = decode_utf8(data, len, chars);
    }
    free(data);

    printf("%s: %lu satir, %lu karakter\n", path, count_lines(chars, n), (unsigned long)n);
    if (utf16)
        puts("Not: dosya UTF-16 kodlamasinda; ASCII olmasi icin UTF-8 olarak kaydedin.");
    bad = report(chars, n);
    free(chars);
    return bad > 0 ? 1 : 0;
}

/* ---- Pano ve konsol (yalnizca Windows) ------------------------------- */

#ifdef _WIN32

/* Panodaki metni kopyalar. Donus: 0 tamam, 1 panoda metin yok,
 * 2 pano acilamadi, 3 bellek yetersiz. */
static int read_clipboard(uint16_t **text, size_t *len)
{
    HANDLE h;
    const uint16_t *p;
    size_t max, n = 0;
    int tries, result = 1;

    *text = NULL;
    *len = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return 1;
    /* Pano o an baska bir programdaysa biraz bekleyip tekrar dene. */
    for (tries = 0; !OpenClipboard(NULL); tries++) {
        if (tries == 20)
            return 2;
        Sleep(50);
    }
    h = GetClipboardData(CF_UNICODETEXT);
    p = h != NULL ? (const uint16_t *)GlobalLock(h) : NULL;
    if (p != NULL) {
        /* Metin ilk NUL karakterinde biter; yapistirilan kisim da odur. */
        max = GlobalSize(h) / sizeof *p;
        while (n < max && p[n] != 0)
            n++;
        *text = malloc((n + 1) * sizeof **text);
        if (*text != NULL) {
            memcpy(*text, p, n * sizeof *p);
            *len = n;
            result = 0;
        } else {
            result = 3;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return result;
}

/* Metnin ilk dolu satirinin basini tirnak icinde yazar; hangi metnin
 * kontrol edildigi gorulsun diye. */
static void print_preview(const uint32_t *t, size_t n)
{
    size_t from = 0, end, to;

    while (from < n && (t[from] == ' ' || t[from] == '\t' || t[from] == '\r' || t[from] == '\n'))
        from++;
    end = from;
    while (end < n && t[end] != '\n')
        end++;
    if (end < n && end > from && t[end - 1] == '\r')
        end--;
    to = end - from > PREVIEW ? from + PREVIEW : end;
    putchar('"');
    print_span(t, n, from, to);
    putchar('"');
    if (to < end)
        fputs("...", stdout);
}

/* Panodaki metni kontrol eder. 0: hepsi ASCII, 1: sorun var, 2: okunamadi. */
static int check_clipboard(void)
{
    static DWORD last_seq;
    DWORD seq = GetClipboardSequenceNumber();
    uint16_t *text;
    uint32_t *chars;
    size_t len, n;
    unsigned long bad;
    int r = read_clipboard(&text, &len);

    if (r == 2) {
        puts("Pano acilamadi; baska bir program kullaniyor olabilir. Tekrar deneyin.");
        return 2;
    }
    if (r == 3) {
        puts("Bellek yetersiz.");
        return 2;
    }
    if (r == 1 || len == 0) {
        free(text);
        puts("Panoda metin yok. Once kontrol edilecek metni kopyalayin (Ctrl+C).");
        return 2;
    }
    chars = len < SIZE_MAX / sizeof *chars ? malloc(len * sizeof *chars) : NULL;
    if (chars == NULL) {
        free(text);
        puts("Bellek yetersiz.");
        return 2;
    }
    n = decode_utf16(text, len, chars);
    free(text);

    fputs("Pano: ", stdout);
    print_preview(chars, n);
    printf(" (%lu satir, %lu karakter)\n", count_lines(chars, n), (unsigned long)n);
    if (seq != 0 && seq == last_seq)
        puts("Not: pano son kontrolden beri degismedi.");
    last_seq = seq;

    bad = report(chars, n);
    free(chars);
    return bad > 0 ? 1 : 0;
}

/* Pencereye cok satirli bir metin yapistirildiysa kalan satirlarini atar:
 * program metni konsoldan degil panodan okur. Girdi 150 ms susana kadar
 * bekler. */
static void discard_input(HANDLE in)
{
    do {
        FlushConsoleInputBuffer(in);
    } while (WaitForSingleObject(in, 150) == WAIT_OBJECT_0);
}

/* Enter'a basilmasini bekler. "q" yazildiysa ya da girdi kapandiysa 0
 * dondurur. */
static int wait_for_enter(HANDLE in)
{
    wchar_t buf[256], head[2] = {0, 0};
    DWORD got, i, seen = 0;

    for (;;) {
        if (!ReadConsoleW(in, buf, (DWORD)COUNT(buf), &got, NULL) || got == 0)
            return 0;
        for (i = 0; i < got && seen < 2; i++)
            head[seen++] = buf[i];
        if (buf[got - 1] == L'\n')
            break;
    }
    discard_input(in);
    if (head[0] == 0x1A) /* Ctrl+Z */
        return 0;
    return !((head[0] == L'q' || head[0] == L'Q') && (head[1] == L'\r' || head[1] == L'\n'));
}

static int run_interactive(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    int status;

    puts("is-this-ascii - ASCII denetleyici. Internete baglanmaz, yalnizca panoyu okur.\n");
    status = check_clipboard();
    if (!GetConsoleMode(in, &mode))
        return status; /* girdi konsol degil (yonlendirme, mintty): tek kontrol */
    for (;;) {
        fputs("\nYeni metni kopyalayip Enter'a basin (cikis: q) > ", stdout);
        fflush(stdout);
        if (!wait_for_enter(in))
            return status;
        status = check_clipboard();
    }
}

/* Program cift tiklamayla ya da dosya uzerine surukle-birak ile acildiysa
 * pencere hemen kapanmasin diye Enter bekler. Bu durumda konsoldaki tek
 * surec bu programdir. */
static void pause_if_own_window(void)
{
    DWORD pids[2];

    if (GetConsoleProcessList(pids, 2) == 1) {
        fputs("\nKapatmak icin Enter'a basin.", stdout);
        fflush(stdout);
        (void)getchar();
    }
}

#endif /* _WIN32 */

/* ---- Ana program ----------------------------------------------------- */

static void setup_output(void)
{
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    /* Renkler icin konsolun ANSI kodlarini islemesini aciyoruz. */
    use_color = GetConsoleMode(out, &mode) &&
                SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    use_color = isatty(STDOUT_FILENO);
#endif
}

static void print_usage(void)
{
    puts("is-this-ascii - metnin yalnizca ASCII karakterlerden olustugunu dogrular.\n"
         "Internete baglanmaz.\n"
         "\n"
         "Kullanim:\n"
#ifdef _WIN32
         "  is-this-ascii           panodaki metni kontrol eder (kopyalayip Enter'a basin)\n"
#endif
         "  is-this-ascii DOSYA...  dosyalari kontrol eder\n"
         "\n"
         "Kabul edilenler: yazdirilabilir ASCII (bosluk dahil), TAB, satir sonu (LF, CRLF).\n"
         "Cikis kodu: 0 = hepsi ASCII, 1 = sorun var, 2 = okunamadi.");
}

int main(int argc, char **argv)
{
    int i, r, status = 0;

    setup_output();
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "/?") == 0)) {
        print_usage();
        return 0;
    }
    if (argc > 1) {
        for (i = 1; i < argc; i++) {
            if (i > 1)
                putchar('\n');
            r = check_file(argv[i]);
            if (r > status)
                status = r;
        }
#ifdef _WIN32
        pause_if_own_window();
#endif
        return status;
    }
#ifdef _WIN32
    return run_interactive();
#else
    print_usage();
    return 2;
#endif
}
