/*
 * stemmer.c — C port of stemmer@2.0.1 (Porter stemmer). See stemmer.h.
 *
 * The reference uses regexes built from the character classes
 *   consonant  = [^aeiou]      vowel  = [aeiouy]
 *   consonants = [^aeiou][^aeiouy]*   vowels = [aeiouy][aeiou]*
 * Under those classes every string decomposes deterministically into strictly
 * alternating consonant/vowel runs (a consonant run ends at the first [aeiouy];
 * a vowel run ends at the first non-[aeiou]). So we classify each character with
 * a small state machine and derive:
 *   - m (measure)     = number of vowel-run -> consonant-run transitions,
 *     which gives gt0 (m>=1), eq1 (m==1) and gt1 (m>=2) exactly.
 *   - hasVowel        = any character classified as a vowel, which is the
 *     reference's `vowelInStem` (*v*) test.
 * The *o test (`consonantLike`) and the fixed suffix tests are checked directly.
 *
 * Suffix steps (2/3/4) pick the longest listed suffix that leaves a non-empty
 * prefix — exactly what the reference's lazy `^(.+?)(a|b|...)$` regexes select.
 */
#include "stemmer.h"

#include <string.h>

/* ---- Character classes (operate on lowercased ASCII) ---------------- */

static int is_aeiou(int c)   { return c=='a'||c=='e'||c=='i'||c=='o'||c=='u'; }
static int is_aeiouy(int c)  { return is_aeiou(c)||c=='y'; }
static int is_aeiouwxy(int c){ return is_aeiouy(c)||c=='w'||c=='x'; }

/* Compute measure m and whether the (prefix) string contains any vowel. */
static void cvstats(const char *s, int len, int *m_out, int *has_vowel) {
    int m = 0, prev = -1;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int t; /* 1 = vowel run, 0 = consonant run */
        if (i == 0)          t = is_aeiou(c) ? 1 : 0;
        else if (prev == 0)  t = is_aeiouy(c) ? 1 : 0;   /* consonant run: [aeiouy] starts a vowel */
        else                 t = is_aeiou(c) ? 1 : 0;    /* vowel run: non-[aeiou] starts a consonant */
        if (t == 1) *has_vowel = 1;
        if (i > 0 && prev == 1 && t == 0) m++;
        prev = t;
    }
    *m_out = m;
}

static int gt0(const char *s, int len) { int m = 0, v = 0; cvstats(s, len, &m, &v); return m >= 1; }
static int gt1(const char *s, int len) { int m = 0, v = 0; cvstats(s, len, &m, &v); return m >= 2; }
static int eq1(const char *s, int len) { int m = 0, v = 0; cvstats(s, len, &m, &v); return m == 1; }
static int has_vowel(const char *s, int len) { int m = 0, v = 0; cvstats(s, len, &m, &v); return v; }

/* *o test: ^[^aeiou][^aeiouy]* [aeiouy] [^aeiouwxy]$ */
static int consonant_like(const char *s, int len) {
    if (len < 3) return 0;
    if (is_aeiouwxy((unsigned char)s[len - 1])) return 0;   /* final [^aeiouwxy] */
    if (!is_aeiouy((unsigned char)s[len - 2])) return 0;     /* penultimate [aeiouy] */
    if (is_aeiou((unsigned char)s[0])) return 0;             /* consonant run starts [^aeiou] */
    for (int i = 1; i <= len - 3; i++)                       /* rest of run is [^aeiouy] */
        if (is_aeiouy((unsigned char)s[i])) return 0;
    return 1;
}

/* ---- Simple suffix predicates --------------------------------------- */

static int ends_with(const char *s, int len, const char *suf) {
    int sl = (int)strlen(suf);
    return len >= sl && memcmp(s + len - sl, suf, (size_t)sl) == 0;
}

/* /^.+?(ss|i)es$/ */
static int sfx_sses_or_ies(const char *s, int len) {
    if (!(len >= 2 && s[len - 2] == 'e' && s[len - 1] == 's')) return 0;
    if (len >= 4 && s[len - 3] == 'i') return 1;                       /* i + es, >=1 before */
    if (len >= 5 && s[len - 3] == 's' && s[len - 4] == 's') return 1;  /* ss + es, >=1 before */
    return 0;
}

/* /^.+?[^s]s$/ */
static int sfx_s(const char *s, int len) {
    return len >= 3 && s[len - 1] == 's' && s[len - 2] != 's';
}

/* /(at|bl|iz)$/ */
static int sfx_at_bl_iz(const char *s, int len) {
    return ends_with(s, len, "at") || ends_with(s, len, "bl") || ends_with(s, len, "iz");
}

/* /([^aeiouylsz])\1$/ — doubled consonant, excluding l, s, z (and vowels/y). */
static int sfx_double(const char *s, int len) {
    if (len < 2 || s[len - 1] != s[len - 2]) return 0;
    int c = (unsigned char)s[len - 1];
    if (is_aeiouy(c) || c == 'l' || c == 's' || c == 'z') return 0;
    return 1;
}

/* ---- Suffix step tables --------------------------------------------- */

typedef struct { const char *suf, *rep; } sufrep;

static const sufrep step2list[] = {
    {"ational","ate"},{"tional","tion"},{"enci","ence"},{"anci","ance"},
    {"izer","ize"},{"bli","ble"},{"alli","al"},{"entli","ent"},{"eli","e"},
    {"ousli","ous"},{"ization","ize"},{"ation","ate"},{"ator","ate"},
    {"alism","al"},{"iveness","ive"},{"fulness","ful"},{"ousness","ous"},
    {"aliti","al"},{"iviti","ive"},{"biliti","ble"},{"logi","log"}
};
static const sufrep step3list[] = {
    {"icate","ic"},{"ative",""},{"alize","al"},{"iciti","ic"},
    {"ical","ic"},{"ful",""},{"ness",""}
};
static const sufrep step4list[] = {
    {"al",""},{"ance",""},{"ence",""},{"er",""},{"ic",""},{"able",""},
    {"ible",""},{"ant",""},{"ement",""},{"ment",""},{"ent",""},{"ou",""},
    {"ism",""},{"ate",""},{"iti",""},{"ous",""},{"ive",""},{"ize",""}
};

/*
 * Find the longest suffix in `list` that `s` ends with and that leaves a
 * non-empty prefix (suffix length <= len-1). Returns its index or -1.
 */
static int find_suffix(const char *s, int len, const sufrep *list, int n) {
    int best = -1, best_len = 0;
    for (int i = 0; i < n; i++) {
        int sl = (int)strlen(list[i].suf);
        if (sl <= len - 1 && sl > best_len && ends_with(s, len, list[i].suf)) {
            best_len = sl; best = i;
        }
    }
    return best;
}

/* Replace the trailing `suf_len` bytes of (out,*n) with `rep`. */
static int apply_rep(char *out, int n, int suf_len, const char *rep) {
    int p = n - suf_len;
    int rl = (int)strlen(rep);
    memcpy(out + p, rep, (size_t)rl);
    return p + rl;
}

/* ---- Main ----------------------------------------------------------- */

int stemmer_stem(const char *word, int len, char *out) {
    int n = len;
    for (int i = 0; i < len; i++) {
        char c = word[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = c;
    }
    if (n < 3) { out[n] = 0; return n; }

    int y_flag = 0;
    if ((unsigned char)out[0] == 'y') { y_flag = 1; out[0] = 'Y'; }

    int m, v;

    /* Step 1a */
    if (sfx_sses_or_ies(out, n)) n -= 2;
    else if (sfx_s(out, n)) n -= 1;

    /* Step 1b */
    if (n >= 4 && ends_with(out, n, "eed")) {
        m = 0; v = 0; cvstats(out, n - 3, &m, &v);
        if (m >= 1) n -= 1;
    } else {
        int suf = 0;
        if (ends_with(out, n, "ed")) suf = 2;
        else if (ends_with(out, n, "ing")) suf = 3;
        if (suf && n - suf >= 1) {
            int p = n - suf;
            if (has_vowel(out, p)) {
                n = p;
                if (sfx_at_bl_iz(out, n)) out[n++] = 'e';
                else if (sfx_double(out, n)) n -= 1;
                else if (consonant_like(out, n)) out[n++] = 'e';
            }
        }
    }

    /* Step 1c */
    if (n >= 2 && out[n - 1] == 'y') {
        if (has_vowel(out, n - 1)) out[n - 1] = 'i';
    }

    /* Step 2 */
    {
        int k = find_suffix(out, n, step2list, (int)(sizeof step2list / sizeof step2list[0]));
        if (k >= 0 && gt0(out, n - (int)strlen(step2list[k].suf)))
            n = apply_rep(out, n, (int)strlen(step2list[k].suf), step2list[k].rep);
    }

    /* Step 3 */
    {
        int k = find_suffix(out, n, step3list, (int)(sizeof step3list / sizeof step3list[0]));
        if (k >= 0 && gt0(out, n - (int)strlen(step3list[k].suf)))
            n = apply_rep(out, n, (int)strlen(step3list[k].suf), step3list[k].rep);
    }

    /* Step 4 */
    {
        int k = find_suffix(out, n, step4list, (int)(sizeof step4list / sizeof step4list[0]));
        if (k >= 0) {
            if (gt1(out, n - (int)strlen(step4list[k].suf))) n -= (int)strlen(step4list[k].suf);
        } else if (n >= 5 && ends_with(out, n, "ion") &&
                   (out[n - 4] == 's' || out[n - 4] == 't')) {
            if (gt1(out, n - 3)) n -= 3;
        }
    }

    /* Step 5 */
    if (n >= 2 && out[n - 1] == 'e') {
        int p = n - 1;
        if (gt1(out, p) || (eq1(out, p) && !consonant_like(out, p))) n = p;
    }
    if (n >= 2 && out[n - 1] == 'l' && out[n - 2] == 'l' && gt1(out, n)) n -= 1;

    if (y_flag) out[0] = 'y';
    out[n] = 0;
    return n;
}
