// Helper program to generate a new secret for use in two-factor
// authentication.
//
// Copyright 2010 Google Inc.
// Author: Markus Gutschke
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "base32.h"
#include "hmac.h"
#include "sha1.h"

#define SECRET                    "/.google_authenticator"
#define SECRET_BITS               160         // Must be divisible by eight
#define VERIFICATION_CODE_MODULUS (1000*1000) // Six digits
#define SCRATCHCODES              5           // Default number of initial scratchcodes
#define MAX_SCRATCHCODES          10          // Max number of initial scratchcodes
#define SCRATCHCODE_LENGTH        8           // Eight digits per scratchcode
#define BYTES_PER_SCRATCHCODE     4           // 32bit of randomness is enough
#define BITS_PER_BASE32_CHAR      5           // Base32 expands space by 8/5

static enum { QR_UNSET=0, QR_NONE, QR_ANSI, QR_ANSI_INVERSE, QR_ANSI_GREY, QR_UTF8, QR_UTF8_INVERSE, QR_UTF8_GREY } qr_mode = QR_UNSET;

static int generateCode(const char *key, unsigned long tm) {
  uint8_t challenge[8];
  for (int i = 8; i--; tm >>= 8) {
    challenge[i] = tm;
  }

  // Estimated number of bytes needed to represent the decoded secret. Because
  // of white-space and separators, this is an upper bound of the real number,
  // which we later get as a return-value from base32_decode()
  int secretLen = (strlen(key) + 7)/8*BITS_PER_BASE32_CHAR;

  // Sanity check, that our secret will fixed into a reasonably-sized static
  // array.
  if (secretLen <= 0 || secretLen > 100) {
    return -1;
  }

  // Decode secret from Base32 to a binary representation, and check that we
  // have at least one byte's worth of secret data.
  uint8_t secret[100];
  if ((secretLen = base32_decode((const uint8_t *)key, secret, secretLen))<1) {
    return -1;
  }

  // Compute the HMAC_SHA1 of the secret and the challenge.
  uint8_t hash[SHA1_DIGEST_LENGTH];
  hmac_sha1(secret, secretLen, challenge, 8, hash, SHA1_DIGEST_LENGTH);

  // Pick the offset where to sample our hash value for the actual verification
  // code.
  const int offset = hash[SHA1_DIGEST_LENGTH - 1] & 0xF;

  // Compute the truncated hash in a byte-order independent loop.
  unsigned int truncatedHash = 0;
  for (int i = 0; i < 4; ++i) {
    truncatedHash <<= 8;
    truncatedHash  |= hash[offset + i];
  }

  // Truncate to a smaller number of digits.
  truncatedHash &= 0x7FFFFFFF;
  truncatedHash %= VERIFICATION_CODE_MODULUS;

  return truncatedHash;
}

// return the user name in heap-allocated buffer.
// Caller frees.
static const char *getUserName(uid_t uid) {
  struct passwd pwbuf, *pw;
  char *buf;
  #ifdef _SC_GETPW_R_SIZE_MAX
  int len = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (len <= 0) {
    len = 4096;
  }
  #else
  int len = 4096;
  #endif
  buf = malloc(len);
  char *user;
  if (getpwuid_r(uid, &pwbuf, buf, len, &pw) || !pw) {
    user = malloc(32);
    snprintf(user, 32, "%d", uid);
  } else {
    user = strdup(pw->pw_name);
    if (!user) {
      perror("malloc()");
      _exit(1);
    }
  }
  free(buf);
  return user;
}

static const char *urlEncode(const char *s) {
  const size_t size = 3 * strlen(s) + 1;
  if (size > 10000) {
    // Anything "too big" is too suspect to let through.
    fprintf(stderr, "Error: Generated URL would be unreasonably large.\n");
    exit(1);
  }
  char *ret = malloc(size);
  char *d = ret;
  do {
    switch (*s) {
    case '%':
    case '&':
    case '?':
    case '=':
    encode:
      snprintf(d, size-(d-ret), "%%%02X", (unsigned char)*s);
      d += 3;
      break;
    default:
      if ((*s && *s <= ' ') || *s >= '\x7F') {
        goto encode;
      }
      *d++ = *s;
      break;
    }
  } while (*s++);
  char* newret = realloc(ret, strlen(ret) + 1);
  if (newret) {
    ret = newret;
  }
  return ret;
}

static const char *getURL(const char *secret, const char *label,
                          const int use_totp, const char *issuer) {
  const char *encodedLabel = urlEncode(label);
  char *url;
  const char totp = use_totp ? 't' : 'h';
  if (asprintf(&url, "otpauth://%cotp/%s?secret=%s", totp, encodedLabel, secret) < 0) {
    fprintf(stderr, "String allocation failed, probably running out of memory.\n");
    _exit(1);
  }

  if (issuer != NULL && strlen(issuer) > 0) {
    // Append to URL &issuer=<issuer>
    const char *encodedIssuer = urlEncode(issuer);
    char *newUrl;
    if (asprintf(&newUrl, "%s&issuer=%s", url, encodedIssuer) < 0) {
      fprintf(stderr, "String allocation failed, probably running out of memory.\n");
      _exit(1);
    }
    free((void *)encodedIssuer);
    free(url);
    url = newUrl;
  }

  free((void *)encodedLabel);
  return url;
}

#define ANSI_RESET        "\x1B[0m"
#define ANSI_BLACKONGREY  "\x1B[30;47m"
#define ANSI_INVERSEOFF   "\x1B[27m"
#define ANSI_INVERSE      "\x1B[7m"
#define UTF8_BOTH         "\xE2\x96\x88"
#define UTF8_TOPHALF      "\xE2\x96\x80"
#define UTF8_BOTTOMHALF   "\xE2\x96\x84"

// Display QR code visually. If not possible, return 0.
static int displayQRCode(const char* url) {
  void *qrencode = dlopen("libqrencode.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!qrencode) {
    qrencode = dlopen("libqrencode.so.3", RTLD_NOW | RTLD_LOCAL);
  }
  if (!qrencode) {
    qrencode = dlopen("libqrencode.so.4", RTLD_NOW | RTLD_LOCAL);
  }
  if (!qrencode) {
    qrencode = dlopen("libqrencode.3.dylib", RTLD_NOW | RTLD_LOCAL);
  }
  if (!qrencode) {
    qrencode = dlopen("libqrencode.4.dylib", RTLD_NOW | RTLD_LOCAL);
  }
  if (!qrencode) {
    return 0;
  }
  typedef struct {
    int version;
    int width;
    unsigned char *data;
  } QRcode;
  QRcode *(*QRcode_encodeString8bit)(const char *, int, int) =
      (QRcode *(*)(const char *, int, int))
      dlsym(qrencode, "QRcode_encodeString8bit");
  void (*QRcode_free)(QRcode *qrcode) =
      (void (*)(QRcode *))dlsym(qrencode, "QRcode_free");
  if (!QRcode_encodeString8bit || !QRcode_free) {
    dlclose(qrencode);
    return 0;
  }
  QRcode *qrcode = QRcode_encodeString8bit(url, 0, 1);
  const char *ptr = (char *)qrcode->data;
  // Output QRCode using ANSI inverting codes. There's also an option to
  // switch to black on grey rather than whatever the current colors are,
  // as well as inverting the current colors. To make sure readers can
  // recognize the code, print a 4-width border around it.
  const int use_inverse_colors = qr_mode == QR_ANSI_INVERSE || qr_mode == QR_UTF8_INVERSE;
  const int use_black_on_grey = qr_mode == QR_ANSI_GREY || qr_mode == QR_UTF8_GREY;
  const char * const color_setup = use_black_on_grey ? ANSI_BLACKONGREY : (use_inverse_colors ? ANSI_INVERSE : "");
  if (qr_mode == QR_ANSI || qr_mode == QR_ANSI_INVERSE || qr_mode == QR_ANSI_GREY) {
    const char * const inverse = use_inverse_colors ? ANSI_INVERSEOFF : ANSI_INVERSE;
    const char * const inverse_off = use_inverse_colors ? ANSI_INVERSE : ANSI_INVERSEOFF;
    fputs(ANSI_RESET, stdout);
    for (int i = 0; i < 2; ++i) {
      fputs(color_setup, stdout);
      for (int x = 0; x < qrcode->width + 4; ++x) {
        fputs("  ", stdout);
      }
      fputs(ANSI_RESET"\n", stdout);
    }
    for (int y = 0; y < qrcode->width; ++y) {
      fputs(color_setup, stdout);
      fputs("    ", stdout);
      int isInverted = 0;
      for (int x = 0; x < qrcode->width; ++x) {
        if (*ptr++ & 1) {
          if (!isInverted) {
            fputs(inverse, stdout);
            isInverted = 1;
          }
        } else {
          if (isInverted) {
            fputs(inverse_off, stdout);
            isInverted = 0;
          }
        }
        fputs("  ", stdout);
      }
      if (isInverted) {
        fputs(inverse_off, stdout);
      }
      fputs("    "ANSI_RESET"\n", stdout);
    }
    for (int i = 0; i < 2; ++i) {
      fputs(color_setup, stdout);
      for (int x = 0; x < qrcode->width + 4; ++x) {
        fputs("  ", stdout);
      }
      fputs(ANSI_RESET"\n", stdout);
    }
  } else {
    // Drawing the QRCode with Unicode block elements is desirable as
    // it makes the display half the size, which is often easier to scan.
    // Unfortunately, many terminal emulators do not display these
    // Unicode characters properly.
    fputs(ANSI_RESET, stdout);
    fputs(color_setup, stdout);
    for (int x = 0; x < qrcode->width + 4; ++x) {
      fputs(" ", stdout);
    }
    fputs(ANSI_RESET"\n", stdout);
    for (int y = 0; y < qrcode->width; y += 2) {
      fputs(color_setup, stdout);
      fputs("  ", stdout);
      for (int x = 0; x < qrcode->width; ++x) {
        const int top = qrcode->data[y*qrcode->width + x] & 1;
        const int bottom = y + 1 < qrcode->width ? qrcode->data[(y + 1) * qrcode->width + x] & 1 : 0;
        if (top) {
          if (bottom) {
            fputs(UTF8_BOTH, stdout);
          } else {
            fputs(UTF8_TOPHALF, stdout);
          }
        } else {
          if (bottom) {
            fputs(UTF8_BOTTOMHALF, stdout);
          } else {
            fputs(" ", stdout);
          }
        }
      }
      fputs("  "ANSI_RESET"\n", stdout);
    }
    fputs(color_setup, stdout);
    for (int x = 0; x < qrcode->width + 4; ++x) {
      fputs(" ", stdout);
    }
    fputs(ANSI_RESET"\n", stdout);
  }
  QRcode_free(qrcode);
  dlclose(qrencode);
  return 1;
}

// Display to the user what they need to provision their app.
static void displayEnrollInfo(const char *secret, const char *label,
                              const int use_totp, const char *issuer) {
  if (qr_mode == QR_NONE) {
    return;
  }
  const char *url = getURL(secret, label, use_totp, issuer);

  // Only newer systems have support for libqrencode. So instead of requiring
  // it at build-time we look for it at run-time. If it cannot be found, the
  // user can still type the code in manually or copy the URL into
  // their browser.
  if (isatty(STDOUT_FILENO)) {
    if (!displayQRCode(url)) {
      printf(
          "Failed to use libqrencode to show QR code visually for scanning.\n"
          "Consider typing the OTP secret into your app manually.\n");
    }
  }

  free((char *)url);
}

// ask for a code. Return code, or some garbage if no number given. That's fine
// because bad data also won't match code.
static int ask_code(const char* msg) {
  char line[128];
  printf("%s ", msg);
  fflush(stdout);

  line[sizeof(line)-1] = 0;
  if (NULL == fgets(line, sizeof line, stdin)) {
    if (errno == 0) {
      printf("\n");
    } else {
      perror("getline()");
    }
    exit(1);
  }

  return strtol(line, NULL, 10);
}

// ask y/n, and return 0 for no, 1 for yes.
static int maybe(const char *msg) {
  printf("\n");
  for (;;) {
    char line[128];
    printf("%s (y/n) ", msg);
    fflush(stdout);

    line[sizeof(line)-1] = 0;
    if (NULL == fgets(line, sizeof(line), stdin)) {
      if (errno == 0) {
        printf("\n");
      } else {
        perror("getline()");
      }
      exit(1);
    }
    switch (line[0]) {
    case 'Y':
    case 'y':
      return 1;
    case 'N':
    case 'n':
      return 0;
    }
  }
}

static char *addOption(char *buf, size_t nbuf, const char *option) {
  assert(strlen(buf) + strlen(option) < nbuf);
  char *scratchCodes = strchr(buf, '\n');
  assert(scratchCodes);
  scratchCodes++;
  memmove(scratchCodes + strlen(option), scratchCodes,
          strlen(scratchCodes) + 1);
  memcpy(scratchCodes, option, strlen(option));
  return buf;
}

static char *maybeAddOption(const char *msg, char *buf, size_t nbuf,
                            const char *option) {
  if (maybe(msg)) {
    buf = addOption(buf, nbuf, option);
  }
  return buf;
}

static void
print_version() {
  puts("google-authenticator "VERSION);
}

static void usage(void) {
  print_version();
  puts(
 "google-authenticator [<options>]\n"
 " -h, --help                     Print this message\n"
 "     --version                  Print version\n"
 " -c, --counter-based            Set up counter-based (HOTP) verification\n"
 " -C, --no-confirm               Don't confirm code. For non-interactive setups\n"
 " -t, --time-based               Set up time-based (TOTP) verification\n"
 " -d, --disallow-reuse           Disallow reuse of previously used TOTP tokens\n"
 " -D, --allow-reuse              Allow reuse of previously used TOTP tokens\n"
 " -f, --force                    Write file without first confirming with user\n"
 " -l, --label=<label>            Override the default label in \"otpauth://\" URL\n"
 " -i, --issuer=<issuer>          Override the default issuer in \"otpauth://\" URL\n"
 " -q, --quiet                    Quiet mode\n"
 " -Q, --qr-mode={NONE,ANSI,ANSI_INVERSE,ANSI_GREY,UTF8,UTF8_INVERSE,UTF8_GREY} QRCode output mode\n"
 " -r, --rate-limit=N             Limit logins to N per every M seconds\n"
 " -R, --rate-time=M              Limit logins to N per every M seconds\n"
 " -u, --no-rate-limit            Disable rate-limiting\n"
 " -s, --secret=<file>            Specify a non-standard file location\n"
 " -S, --step-size=S              Set interval between token refreshes\n"
 " -w, --window-size=W            Set window of concurrently valid codes\n"
 " -W, --minimal-window           Disable window of concurrently valid codes\n"
 " -e, --emergency-codes=N        Number of emergency codes to generate");
}

int main(int argc, char *argv[]) {
  uint8_t buf[SECRET_BITS/8 + MAX_SCRATCHCODES*BYTES_PER_SCRATCHCODE];
  static const char hotp[]      = "\" HOTP_COUNTER 1\n";
  static const char totp[]      = "\" TOTP_AUTH\n";
  static const char disallow[]  = "\" DISALLOW_REUSE\n";
  static const char step[]      = "\" STEP_SIZE 30\n";
  static const char window[]    = "\" WINDOW_SIZE 17\n";
  static const char ratelimit[] = "\" RATE_LIMIT 3 30\n";
  char secret[(SECRET_BITS + BITS_PER_BASE32_CHAR-1)/BITS_PER_BASE32_CHAR +
              1 /* newline */ +
              sizeof(hotp) +  // hotp and totp are mutually exclusive.
              sizeof(disallow) +
              sizeof(step) +
              sizeof(window) +
              sizeof(ratelimit) + 5 + // NN MMM (total of five digits)
              SCRATCHCODE_LENGTH*(MAX_SCRATCHCODES + 1 /* newline */) +
              1 /* NUL termination character */];

  enum { ASK_MODE, HOTP_MODE, TOTP_MODE } mode = ASK_MODE;
  enum { ASK_REUSE, DISALLOW_REUSE, ALLOW_REUSE } reuse = ASK_REUSE;
  int force = 0, quiet = 0;
  int r_limit = 0, r_time = 0;
  char *secret_fn = NULL;
  char *label = NULL;
  char *issuer = NULL;
  int step_size = 0;
  int confirm = 1;
  int window_size = 0;
  int emergency_codes = -1;
  int idx;
  for (;;) {
    static const char optstring[] = "+hcCtdDfl:i:qQ:r:R:us:S:w:We:";
    static struct option options[] = {
      { "help",             0, 0, 'h' },
      { "version",          0, 0, 0},
      { "counter-based",    0, 0, 'c' },
      { "no-confirm",       0, 0, 'C' },
      { "time-based",       0, 0, 't' },
      { "disallow-reuse",   0, 0, 'd' },
      { "allow-reuse",      0, 0, 'D' },
      { "force",            0, 0, 'f' },
      { "label",            1, 0, 'l' },
      { "issuer",           1, 0, 'i' },
      { "quiet",            0, 0, 'q' },
      { "qr-mode",          1, 0, 'Q' },
      { "rate-limit",       1, 0, 'r' },
      { "rate-time",        1, 0, 'R' },
      { "no-rate-limit",    0, 0, 'u' },
      { "secret",           1, 0, 's' },
      { "step-size",        1, 0, 'S' },
      { "window-size",      1, 0, 'w' },
      { "minimal-window",   0, 0, 'W' },
      { "emergency-codes",  1, 0, 'e' },
      { 0,                  0, 0,  0  }
    };
    idx = -1;
    const int c = getopt_long(argc, argv, optstring, options, &idx);
    if (c > 0) {
      for (int i = 0; options[i].name; i++) {
        if (options[i].val == c) {
          idx = i;
          break;
        }
      }
    } else if (c < 0) {
      break;
    }
    if (idx-- <= 0) {
      // Help (or invalid argument)
    err:
      usage();
      if (idx < -1) {
        fprintf(stderr, "Failed to parse command line\n");
        _exit(1);
      }
      exit(0);
    } else if (!idx--) {
      // --version
      print_version();
      exit(0);
    } else if (!idx--) {
      // counter-based, -c
      if (mode != ASK_MODE) {
        fprintf(stderr, "Duplicate -c and/or -t option detected\n");
        _exit(1);
      }
      if (reuse != ASK_REUSE) {
      reuse_err:
        fprintf(stderr, "Reuse of tokens is not a meaningful parameter "
                "in counter-based mode\n");
        _exit(1);
      }
      mode = HOTP_MODE;
    } else if (!idx--) {
      // don't confirm code provisioned, -C
      confirm = 0;
    } else if (!idx--) {
      // time-based
      if (mode != ASK_MODE) {
        fprintf(stderr, "Duplicate -c and/or -t option detected\n");
        _exit(1);
      }
      mode = TOTP_MODE;
    } else if (!idx--) {
      // disallow-reuse
      if (reuse != ASK_REUSE) {
        fprintf(stderr, "Duplicate -d and/or -D option detected\n");
        _exit(1);
      }
      if (mode == HOTP_MODE) {
        goto reuse_err;
      }
      reuse = DISALLOW_REUSE;
    } else if (!idx--) {
      // allow-reuse
      if (reuse != ASK_REUSE) {
        fprintf(stderr, "Duplicate -d and/or -D option detected\n");
        _exit(1);
      }
      if (mode == HOTP_MODE) {
        goto reuse_err;
      }
      reuse = ALLOW_REUSE;
    } else if (!idx--) {
      // force
      if (force) {
        fprintf(stderr, "Duplicate -f option detected\n");
        _exit(1);
      }
      force = 1;
    } else if (!idx--) {
      // label
      if (label) {
        fprintf(stderr, "Duplicate -l option detected\n");
        _exit(1);
      }
      label = strdup(optarg);
    } else if (!idx--) {
      // issuer
      if (issuer) {
        fprintf(stderr, "Duplicate -i option detected\n");
        _exit(1);
      }
      issuer = strdup(optarg);
    } else if (!idx--) {
      // quiet
      if (quiet) {
        fprintf(stderr, "Duplicate -q option detected\n");
        _exit(1);
      }
      quiet = 1;
    } else if (!idx--) {
      // qr-mode
      if (qr_mode != QR_UNSET) {
        fprintf(stderr, "Duplicate -Q option detected\n");
        _exit(1);
      }
      if (!strcasecmp(optarg, "none")) {
        qr_mode = QR_NONE;
      } else if (!strcasecmp(optarg, "ANSI_INVERSE") || !strcasecmp(optarg, "ANSI-INVERSE")) {
        qr_mode = QR_ANSI_INVERSE;
      } else if (!strcasecmp(optarg, "ANSI_GREY") || !strcasecmp(optarg, "ANSI-GREY")) {
        qr_mode = QR_ANSI_GREY;
      } else if (!strcasecmp(optarg, "ANSI")) {
        qr_mode = QR_ANSI;
      } else if (!strcasecmp(optarg, "UTF8_INVERSE") || !strcasecmp(optarg, "UTF8-INVERSE")) {
        qr_mode = QR_UTF8_INVERSE;
      } else if (!strcasecmp(optarg, "UTF8_GREY") || !strcasecmp(optarg, "UTF8-GREY")) {
        qr_mode = QR_UTF8_GREY;
      } else if (!strcasecmp(optarg, "UTF8")) {
        qr_mode = QR_UTF8;
      } else {
        fprintf(stderr, "Invalid qr-mode \"%s\"\n", optarg);
        _exit(1);
      }
    } else if (!idx--) {
      // rate-limit
      if (r_limit > 0) {
        fprintf(stderr, "Duplicate -r option detected\n");
        _exit(1);
      } else if (r_limit < 0) {
        fprintf(stderr, "-u is mutually exclusive with -r\n");
        _exit(1);
      }
      char *endptr;
      errno = 0;
      const long l = strtol(optarg, &endptr, 10);
      if (errno || endptr == optarg || *endptr || l < 1 || l > 10) {
        fprintf(stderr, "-r requires an argument in the range 1..10\n");
        _exit(1);
      }
      r_limit = (int)l;
    } else if (!idx--) {
      // rate-time
      if (r_time > 0) {
        fprintf(stderr, "Duplicate -R option detected\n");
        _exit(1);
      } else if (r_time < 0) {
        fprintf(stderr, "-u is mutually exclusive with -R\n");
        _exit(1);
      }
      char *endptr;
      errno = 0;
      const long l = strtol(optarg, &endptr, 10);
      if (errno || endptr == optarg || *endptr || l < 15 || l > 600) {
        fprintf(stderr, "-R requires an argument in the range 15..600\n");
        _exit(1);
      }
      r_time = (int)l;
    } else if (!idx--) {
      // no-rate-limit
      if (r_limit > 0 || r_time > 0) {
        fprintf(stderr, "-u is mutually exclusive with -r/-R\n");
        _exit(1);
      }
      if (r_limit < 0) {
        fprintf(stderr, "Duplicate -u option detected\n");
        _exit(1);
      }
      r_limit = r_time = -1;
    } else if (!idx--) {
      // secret
      if (secret_fn) {
        fprintf(stderr, "Duplicate -s option detected\n");
        _exit(1);
      }
      if (!*optarg) {
        fprintf(stderr, "-s must be followed by a filename\n");
        _exit(1);
      }
      secret_fn = strdup(optarg);
      if (!secret_fn) {
        perror("malloc()");
        _exit(1);
      }
    } else if (!idx--) {
      // step-size
      if (step_size) {
        fprintf(stderr, "Duplicate -S option detected\n");
        _exit(1);
      }
      char *endptr;
      errno = 0;
      const long l = strtol(optarg, &endptr, 10);
      if (errno || endptr == optarg || *endptr || l < 1 || l > 60) {
        fprintf(stderr, "-S requires an argument in the range 1..60\n");
        _exit(1);
      }
      step_size = (int)l;
    } else if (!idx--) {
      // window-size
      if (window_size) {
        fprintf(stderr, "Duplicate -w/-W option detected\n");
        _exit(1);
      }
      char *endptr;
      errno = 0;
      const long l = strtol(optarg, &endptr, 10);
      if (errno || endptr == optarg || *endptr || l < 1 || l > 21) {
        fprintf(stderr, "-w requires an argument in the range 1..21\n");
        _exit(1);
      }
      window_size = (int)l;
    } else if (!idx--) {
      // minimal-window
      if (window_size) {
        fprintf(stderr, "Duplicate -w/-W option detected\n");
        _exit(1);
      }
      window_size = -1;
    } else if (!idx--) {
      // emergency-codes
      if (emergency_codes >= 0) {
        fprintf(stderr, "Duplicate -e option detected\n");
        _exit(1);
      }
      char *endptr;
      errno = 0;
      long l = strtol(optarg, &endptr, 10);
      if (errno || endptr == optarg || *endptr || l < 0 || l > MAX_SCRATCHCODES) {
        fprintf(stderr, "-e requires an argument in the range 0..%d\n", MAX_SCRATCHCODES);
        _exit(1);
      }
      emergency_codes = (int)l;
    } else {
      fprintf(stderr, "Error\n");
      _exit(1);
    }
  }
  if (qr_mode == QR_UNSET) {
    qr_mode = QR_ANSI; // most universal option
  }
  idx = -1;
  if (optind != argc) {
    goto err;
  }
  if (reuse != ASK_REUSE && mode != TOTP_MODE) {
    fprintf(stderr, "Must select time-based mode, when using -d or -D\n");
    _exit(1);
  }
  if ((r_time && !r_limit) || (!r_time && r_limit)) {
    fprintf(stderr, "Must set -r when setting -R, and vice versa\n");
    _exit(1);
  }
  if (emergency_codes < 0) {
    emergency_codes = SCRATCHCODES;
  }
  if (!label) {
    const uid_t uid = getuid();
    const char *user = getUserName(uid);
    char hostname[128] = { 0 };
    if (gethostname(hostname, sizeof(hostname)-1)) {
      strcpy(hostname, "unix");
    }
    label = strcat(strcat(strcpy(malloc(strlen(user) + strlen(hostname) + 2),
                                 user), "@"), hostname);
    free((char *)user);
  }
  if (!issuer) {
    char hostname[128] = { 0 };
    if (gethostname(hostname, sizeof(hostname)-1)) {
      strcpy(hostname, "unix");
    }

    issuer = strdup(hostname);
  }
  // Not const because 'fd' is reused. TODO.
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    perror("Failed to open \"/dev/urandom\"");
    return 1;
  }
  if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
  urandom_failure:
    perror("Failed to read from \"/dev/urandom\"");
    return 1;
  }

  base32_encode(buf, SECRET_BITS/8, (uint8_t *)secret, sizeof(secret));
  int use_totp;
  if (mode == ASK_MODE) {
    use_totp = maybe("Do you want authentication tokens to be time-based");
  } else {
    use_totp = mode == TOTP_MODE;
  }
  if (!quiet) {
    displayEnrollInfo(secret, label, use_totp, issuer);
    printf("Your new secret key is: %s\n", secret);

    // Confirm code.
    if (confirm && use_totp) {
      for (;;) {
        const int test_code = ask_code("Enter code from app (-1 to skip):");
        if (test_code < 0) {
          printf("Code confirmation skipped\n");
          break;
        }
        const unsigned long tm = time(NULL)/(step_size ? step_size : 30);
        const int correct_code = generateCode(secret, tm);
        if (test_code == correct_code) {
          printf("Code confirmed\n");
          break;
        }
        printf("Code incorrect (correct code %06d). Try again.\n",
               correct_code);
      }
    } else {
      const unsigned long tm = 1;
      printf("Your verification code for code %lu is %06d\n",
             tm, generateCode(secret, tm));
    }
    printf("Your emergency scratch codes are:\n");
  }
  free(label);
  free(issuer);
  strcat(secret, "\n");
  if (use_totp) {
    strcat(secret, totp);
  } else {
    strcat(secret, hotp);
  }
  for (int i = 0; i < emergency_codes; ++i) {
  new_scratch_code:;
    int scratch = 0;
    for (int j = 0; j < BYTES_PER_SCRATCHCODE; ++j) {
      scratch = 256*scratch + buf[SECRET_BITS/8 + BYTES_PER_SCRATCHCODE*i + j];
    }
    int modulus = 1;
    for (int j = 0; j < SCRATCHCODE_LENGTH; j++) {
      modulus *= 10;
    }
    scratch = (scratch & 0x7FFFFFFF) % modulus;
    if (scratch < modulus/10) {
      // Make sure that scratch codes are always exactly eight digits. If they
      // start with a sequence of zeros, just generate a new scratch code.
      if (read(fd, buf + (SECRET_BITS/8 + BYTES_PER_SCRATCHCODE*i),
               BYTES_PER_SCRATCHCODE) != BYTES_PER_SCRATCHCODE) {
        goto urandom_failure;
      }
      goto new_scratch_code;
    }
    if (!quiet) {
      printf("  %08d\n", scratch);
    }
    snprintf(strrchr(secret, '\000'), sizeof(secret) - strlen(secret),
             "%08d\n", scratch);
  }
  close(fd);
  if (!secret_fn) {
    const char *home = getenv("HOME");
    if (!home || *home != '/') {
      fprintf(stderr, "Cannot determine home directory\n");
      return 1;
    }
    secret_fn = malloc(strlen(home) + strlen(SECRET) + 1);
    if (!secret_fn) {
      perror("malloc()");
      _exit(1);
    }
    strcat(strcpy(secret_fn, home), SECRET);
  }
  if (!force) {
    char s[1024];
    snprintf(s, sizeof s, "Do you want me to update your \"%s\" file?",
             secret_fn);
    if (!maybe(s)) {
      exit(0);
    }
  }

  const int size = strlen(secret_fn) + 3;
  char* tmp_fn = malloc(size);
  if (!tmp_fn) {
    perror("malloc()");
    _exit(1);
  }
  snprintf(tmp_fn, size, "%s~", secret_fn);

  // Add optional flags.
  if (use_totp) {
    if (reuse == ASK_REUSE) {
      maybeAddOption("Do you want to disallow multiple uses of the same "
                     "authentication\ntoken? This restricts you to one login "
                     "about every 30s, but it increases\nyour chances to "
                     "notice or even prevent man-in-the-middle attacks",
                     secret, sizeof(secret), disallow);
    } else if (reuse == DISALLOW_REUSE) {
      addOption(secret, sizeof(secret), disallow);
    }
    if (step_size) {
      char s[80];
      snprintf(s, sizeof s, "\" STEP_SIZE %d\n", step_size);
      addOption(secret, sizeof(secret), s);
    }
    if (!window_size) {
      maybeAddOption("By default, a new token is generated every 30 seconds by"
                     " the mobile app.\nIn order to compensate for possible"
                     " time-skew between the client and the server,\nwe allow"
                     " an extra token before and after the current time. This"
                     " allows for a\ntime skew of up to 30 seconds between"
                     " authentication server and client. If you\nexperience"
                     " problems with poor time synchronization, you can"
                     " increase the window\nfrom its default size of 3"
                     " permitted codes (one previous code, the current\ncode,"
                     " the next code) to 17 permitted codes (the 8 previous"
                     " codes, the current\ncode, and the 8 next codes)."
                     " This will permit for a time skew of up to 4 minutes"
                     "\nbetween client and server."
                     "\nDo you want to do so?",
                     secret, sizeof(secret), window);
    } else {
      char s[80];
      // TODO: Should 3 really be the minimal window size for TOTP?
      // If so, the code should not allow -w=1 here.
      snprintf(s, sizeof s, "\" WINDOW_SIZE %d\n", window_size > 0 ? window_size : 3);
      addOption(secret, sizeof(secret), s);
    }
  } else {
    // Counter based.
    if (!window_size) {
      maybeAddOption("By default, three tokens are valid at any one time.  "
                     "This accounts for\ngenerated-but-not-used tokens and "
                     "failed login attempts. In order to\ndecrease the "
                     "likelihood of synchronization problems, this window "
                     "can be\nincreased from its default size of 3 to 17. Do "
                     "you want to do so?",
                     secret, sizeof(secret), window);
    } else {
      char s[80];
      snprintf(s, sizeof s, "\" WINDOW_SIZE %d\n", window_size > 0 ? window_size : 1);
      addOption(secret, sizeof(secret), s);
    }
  }
  if (!r_limit && !r_time) {
    maybeAddOption("If the computer that you are logging into isn't hardened "
                   "against brute-force\nlogin attempts, you can enable "
                   "rate-limiting for the authentication module.\nBy default, "
                   "this limits attackers to no more than 3 login attempts "
                   "every 30s.\nDo you want to enable rate-limiting?",
                   secret, sizeof(secret), ratelimit);
  } else if (r_limit > 0 && r_time > 0) {
    char s[80];
    snprintf(s, sizeof s, "\" RATE_LIMIT %d %d\n", r_limit, r_time);
    addOption(secret, sizeof(secret), s);
  }

  fd = open(tmp_fn, O_WRONLY|O_EXCL|O_CREAT|O_NOFOLLOW|O_TRUNC, 0400);
  if (fd < 0) {
    fprintf(stderr, "Failed to create \"%s\" (%s)",
            secret_fn, strerror(errno));
    goto errout;
  }
  if (write(fd, secret, strlen(secret)) != (ssize_t)strlen(secret) ||
      rename(tmp_fn, secret_fn)) {
    perror("Failed to write new secret");
    unlink(secret_fn);
    goto errout;
  }

  free(tmp_fn);
  free(secret_fn);
  close(fd);

  return 0;

errout:
  if (fd > 0) {
    close(fd);
  }
  free(secret_fn);
  free(tmp_fn);
  return 1;
}
/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */
