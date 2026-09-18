/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  Commandline .map rendering utility, mostly for testing.
 * Author:   Steve Lime and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2005 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "../mapserver.h"
#include "../maptime.h"

#include "limits.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * Is stdin something other than a terminal (a pipe, a file, /dev/null)?
 * Used to decide whether an omitted -m means "read stdin" or "you forgot
 * an argument" — without it, a bare map2img would block forever.
 */
static int stdinIsRedirected(void) {
#ifdef _WIN32
  return !_isatty(_fileno(stdin));
#else
  return !isatty(fileno(stdin));
#endif
}

/**
 * Check if the required number of arguments are available for the
 * parsed option or otherwise print an error message and exit.
 *
 * @param option option name for the error message
 * @param num_required_arguments number of arguments required by option
 * @param num_remaining_arguments number of arguments following the option
 * @param config optional config object to free if we exit
 * @param map optional map object to free if we exit
 */
static void hasMoreArgumentsOrExit(const char *option,
                                   int num_required_arguments,
                                   int num_remaining_arguments,
                                   configObj *config, mapObj *map) {
  (void)map;
  if (num_remaining_arguments < num_required_arguments) {
    if (num_required_arguments == 1)
      fprintf(stderr, "Argument %s requires an additional argument.\n", option);
    else
      fprintf(stderr, "Argument %s needs %i space separated arguments.\n",
              option, num_required_arguments);
    msCleanup();
    msFreeConfig(config);
    exit(1);
  }
}

/**
 * Read stdin into a NUL-terminated buffer for msLoadMapFromString().
 * Rejects embedded NUL bytes, which would silently truncate the Mapfile
 * mid-parse. Returns NULL on allocation, size, or input error. Caller frees.
 */
static char *readStdinToBuffer(void) {
  size_t capacity = 65536, length = 0;
  char *buffer = NULL;

#ifdef _WIN32
  /* Avoid CRLF translation mangling offsets in the buffer. Not restored:
   * stdin is read once, to EOF, and never touched again.
   */
  _setmode(_fileno(stdin), _O_BINARY);
#endif

  buffer = (char *)malloc(capacity);
  if (!buffer) {
    fprintf(stderr, "Allocation failed reading Mapfile from stdin.\n");
    return NULL;
  }

  for (;;) {
    size_t n;

    /* capacity >= length always, so this cannot wrap. */
    if (capacity - length < 4097) {
      char *tmp;
      if (capacity > SIZE_MAX / 2) {
        fprintf(stderr, "Mapfile on stdin is too large.\n");
        goto cleanup;
      }
      tmp = (char *)realloc(buffer, capacity * 2);
      if (!tmp) {
        fprintf(stderr, "Allocation failed reading Mapfile from stdin.\n");
        goto cleanup;
      }
      buffer = tmp;
      capacity *= 2;
    }

    n = fread(buffer + length, 1, 4096, stdin);

    if (memchr(buffer + length, '\0', n) != NULL) {
      fprintf(stderr, "Mapfile on stdin contains NUL bytes; expected plain "
                      "text. Check the encoding of the piped input.\n");
      goto cleanup;
    }

    length += n;

    if (n < 4096) {
      if (ferror(stdin)) {
        fprintf(stderr, "Error reading Mapfile from stdin.\n");
        goto cleanup;
      }
      break;
    }
  }
  buffer[length] = '\0';

  if (length == 0) {
    fprintf(stderr, "No Mapfile received on stdin.\n");
    goto cleanup;
  }

  /* A UTF-8 BOM breaks the lexer on the first token. */
  if (length >= 3 && memcmp(buffer, "\xEF\xBB\xBF", 3) == 0)
    memmove(buffer, buffer + 3, length - 3 + 1);

  return buffer;

cleanup:
  free(buffer);
  return NULL;
}

int main(int argc, char *argv[]) {
  int i, j, k;

  mapObj *map = NULL;
  imageObj *image = NULL;
  configObj *config = NULL;

  char **layers = NULL;
  int num_layers = 0;

  int layer_found = 0;

  char *outfile = NULL; /* no -o sends image to STDOUT */

  int iterations = 1;
  int draws = 0;

  /* ---- output version info and exit --- */
  if (argc > 1 && strcmp(argv[1], "-v") == 0) {
    printf("%s\n", msGetVersion());
    exit(0);
  }

  /* ---- check the number of arguments, return syntax if not correct ---- */
  if (argc < 3 && !stdinIsRedirected()) {
    fprintf(stdout, "\nPurpose: convert a mapfile to an image\n\n");
    fprintf(stdout, "Syntax: map2img -m mapfile [-o image] [-e minx miny maxx "
                    "maxy] [-s sizex sizey]\n"
                    "               [-l \"layer1 [layers2...]\"] [-i format]\n"
                    "               [-all_debug n] [-map_debug n] "
                    "[-layer_debug n] [-p n] [-c n] [-d layername datavalue]\n"
                    "               [-conf filename]\n");
    fprintf(stdout, "  -m mapfile: Mapfile to operate on - required. Use '-' "
                    "to read the mapfile from stdin. If -m is omitted and "
                    "stdin is not a terminal, the Mapfile is read from "
                    "stdin\n");
    fprintf(stdout, "  -mappath path: base directory for relative SHAPEPATH, "
                    "FONTSET, SYMBOLSET and INCLUDE paths\n");
    fprintf(
        stdout,
        "  -i format: Override the IMAGETYPE value to pick output format\n");
    fprintf(stdout, "  -o image: output filename (stdout if not provided)\n");
    fprintf(stdout, "  -e minx miny maxx maxy: extents to render\n");
    fprintf(stdout, "  -s sizex sizey: output image size\n");
    fprintf(stdout, "  -l layers: layers / groups to enable - make sure they "
                    "are quoted and space separated if more than one listed\n");
    fprintf(stdout, "  -all_debug n: Set debug level for map and all layers\n");
    fprintf(stdout, "  -map_debug n: Set map debug level\n");
    fprintf(stdout, "  -layer_debug layer_name n: Set layer debug level\n");
    fprintf(stdout, "  -c n: draw map n number of times\n");
    fprintf(stdout, "  -p n: pause for n seconds after reading the map\n");
    fprintf(stdout, "  -d layername datavalue: change DATA value for layer\n");
    fprintf(
        stdout,
        "  -conf filename: filename of the MapServer configuration file.\n");
    exit(0);
  }

  if (msSetup() != MS_SUCCESS) {
    msWriteError(stderr);
    exit(1);
  }

  bool some_debug_requested = FALSE;
  const char *config_filename = NULL;
  const char *mappath = NULL;
  char *mapfile_buffer = NULL;
  int have_m_option = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0) { /* user specified number of draws */
      hasMoreArgumentsOrExit("-c", 1, argc - i - 1, config, map);
      iterations = atoi(argv[i + 1]);
      if (iterations < 0 || iterations > INT_MAX - 1) {
        printf("Invalid number of iterations");
        return 1;
      }
      printf("We will draw %d times...\n", iterations);
      continue;
    }

    if (strcmp(argv[i], "-all_debug") == 0) { /* global debug */
      hasMoreArgumentsOrExit("-all_debug", 1, argc - i - 1, config, map);
      int debug_level = atoi(argv[++i]);

      some_debug_requested = TRUE;

      msSetGlobalDebugLevel(debug_level);

      continue;
    }

    if (i < argc - 1 && (strcmp(argv[i], "-map_debug") == 0 ||
                         strcmp(argv[i], "-layer_debug") == 0)) {

      some_debug_requested = TRUE;
      continue;
    }

    if (strcmp(argv[i], "-conf") == 0) {
      hasMoreArgumentsOrExit("-conf", 1, argc - i - 1, config, map);
      config_filename = argv[i + 1];
      ++i;
      continue;
    }

    if (strcmp(argv[i], "-mappath") == 0) {
      hasMoreArgumentsOrExit("-mappath", 1, argc - i - 1, config, map);
      mappath = argv[i + 1];
      ++i;
      continue;
    }

    /* Read a piped Mapfile once outside the draws loop */
    if (strcmp(argv[i], "-m") == 0) {
      hasMoreArgumentsOrExit("-m", 1, argc - i - 1, config, map);
      have_m_option = 1;
      if (strcmp(argv[i + 1], "-") == 0) {
        mapfile_buffer = readStdinToBuffer();
        if (!mapfile_buffer) {
          msCleanup();
          exit(1);
        }
      }
      ++i;
      continue;
    }
  }

  /* No -m option supplied, but something is piped in, so read the Mapfile from
   * stdin. */
  if (!have_m_option && stdinIsRedirected()) {
    mapfile_buffer = readStdinToBuffer();
    if (!mapfile_buffer) {
      msCleanup();
      exit(1);
    }
  }

  if (some_debug_requested) {
    /* Send output to stderr by default */
    if (msGetErrorFile() == NULL)
      msSetErrorFile("stderr", NULL);
  }

  config = msLoadConfig(config_filename);

  for (draws = 0; draws < iterations; draws++) {

    struct mstimeval requeststarttime, requestendtime;

    if (msGetGlobalDebugLevel() >= MS_DEBUGLEVEL_TUNING)
      msGettimeofday(&requeststarttime, NULL);

    /* Use PROJ_DATA/PROJ_LIB env vars if set */
    msProjDataInitFromEnv();

    /* Use MS_ERRORFILE and MS_DEBUGLEVEL env vars if set */
    if (msDebugInitFromEnv() != MS_SUCCESS) {
      msWriteError(stderr);
      msCleanup();
      msFreeConfig(config);
      free(mapfile_buffer);
      exit(1);
    }

    if (mapfile_buffer) {
      map = msLoadMapFromString(mapfile_buffer, mappath, config);
    } else {
      for (i = 1; i < argc; i++) { /* find the map file */
        if (strcmp(argv[i], "-m") == 0) {
          hasMoreArgumentsOrExit("-m", 1, argc - i - 1, config, map);
          map = msLoadMap(argv[i + 1], mappath, config);
          break;
        }
      }
    }

    if (!map) {
      if (mapfile_buffer || have_m_option)
        msWriteError(stderr); /* Mapfile loading failed */
      else
        fprintf(stderr, "No Mapfile specified. Use -m <mapfile>, -m - to read "
                        "from stdin, or pipe a Mapfile in.\n");
      msCleanup();
      msFreeConfig(config);
      free(mapfile_buffer);
      exit(1);
    }

    msApplyDefaultSubstitutions(map);
    msApplyStyleItemsToLayers(map);

    for (i = 1; i < argc; i++) { /* Step though the user arguments */

      if (strcmp(argv[i], "-m") == 0) { /* skip it */
        i += 1;
      }

      if (strcmp(argv[i], "-p") == 0) {
        hasMoreArgumentsOrExit("-p", 1, argc - i - 1, config, map);
        int pause_length = atoi(argv[i + 1]);
        time_t start_time = time(NULL);

        printf("Start pause of %d seconds.\n", pause_length);
        while (time(NULL) < start_time + pause_length) {
        }
        printf("Done pause.\n");

        i += 1;
      }

      if (strcmp(argv[i], "-o") == 0) { /* load the output image filename */
        hasMoreArgumentsOrExit("-o", 1, argc - i - 1, config, map);
        outfile = argv[i + 1];
        i += 1;
      }

      if (strcmp(argv[i], "-i") == 0) {
        hasMoreArgumentsOrExit("-i", 1, argc - i - 1, config, map);
        outputFormatObj *format;

        format = msSelectOutputFormat(map, argv[i + 1]);

        if (format == NULL)
          printf("No such OUTPUTFORMAT as %s.\n", argv[i + 1]);
        else {
          msFree((char *)map->imagetype);
          map->imagetype = msStrdup(argv[i + 1]);
          msApplyOutputFormat(&(map->outputformat), format, MS_NOOVERRIDE);
        }
        i += 1;
      }

      if (strcmp(argv[i], "-d") == 0) { /* swap layer data */
        hasMoreArgumentsOrExit("-d", 2, argc - i - 1, config, map);
        for (j = 0; j < map->numlayers; j++) {
          if (strcmp(GET_LAYER(map, j)->name, argv[i + 1]) == 0) {
            free(GET_LAYER(map, j)->data);
            GET_LAYER(map, j)->data = msStrdup(argv[i + 2]);
            break;
          }
        }
        i += 2;
      }

      if (strcmp(argv[i], "-all_debug") == 0) { /* global debug */
        hasMoreArgumentsOrExit("-all_debug", 1, argc - i - 1, config, map);
        int debug_level = atoi(argv[++i]);

        /* msSetGlobalDebugLevel() already called. Just need to force debug
         * level in map and all layers
         */
        map->debug = debug_level;
        for (j = 0; j < map->numlayers; j++) {
          GET_LAYER(map, j)->debug = debug_level;
        }
      }

      if (strcmp(argv[i], "-map_debug") == 0) { /* debug */
        hasMoreArgumentsOrExit("-map_debug", 1, argc - i - 1, config, map);
        map->debug = atoi(argv[++i]);
      }

      if (strcmp(argv[i], "-layer_debug") == 0) { /* debug */
        hasMoreArgumentsOrExit("-layer_debug", 2, argc - i - 1, config, map);
        const char *layer_name = argv[++i];
        int debug_level = atoi(argv[++i]);
        int got_layer = 0;

        for (j = 0; j < map->numlayers; j++) {
          if (strcmp(GET_LAYER(map, j)->name, layer_name) == 0) {
            GET_LAYER(map, j)->debug = debug_level;
            got_layer = 1;
          }
        }
        if (!got_layer)
          fprintf(stderr,
                  " Did not find layer '%s' from -layer_debug switch.\n",
                  layer_name);
      }

      if (strcmp(argv[i], "-e") == 0) { /* change extent */
        hasMoreArgumentsOrExit("-e", 4, argc - i - 1, config, map);
        map->extent.minx = atof(argv[i + 1]);
        map->extent.miny = atof(argv[i + 2]);
        map->extent.maxx = atof(argv[i + 3]);
        map->extent.maxy = atof(argv[i + 4]);
        i += 4;
      }

      if (strcmp(argv[i], "-s") == 0) {
        hasMoreArgumentsOrExit("-s", 2, argc - i - 1, config, map);
        msMapSetSize(map, atoi(argv[i + 1]), atoi(argv[i + 2]));
        i += 2;
      }

      if (strcmp(argv[i], "-l") == 0) { /* load layer list */
        hasMoreArgumentsOrExit("-l", 1, argc - i - 1, config, map);
        layers = msStringSplit(argv[i + 1], ' ', &(num_layers));

        for (j = 0; j < num_layers; j++) { /* loop over -l */
          layer_found = 0;
          for (k = 0; k < map->numlayers; k++) {
            if ((GET_LAYER(map, k)->name &&
                 strcasecmp(GET_LAYER(map, k)->name, layers[j]) == 0) ||
                (GET_LAYER(map, k)->group &&
                 strcasecmp(GET_LAYER(map, k)->group, layers[j]) == 0)) {
              layer_found = 1;
              break;
            }
          }
          if (layer_found == 0) {
            fprintf(stderr, "Layer (-l) \"%s\" not found\n", layers[j]);
            msFreeCharArray(layers, num_layers);
            msFreeMap(map);
            msCleanup();
            msFreeConfig(config);
            free(mapfile_buffer);
            exit(1);
          }
        }

        for (j = 0; j < map->numlayers; j++) {
          if (GET_LAYER(map, j)->status == MS_DEFAULT)
            continue;
          else {
            GET_LAYER(map, j)->status = MS_OFF;
            for (k = 0; k < num_layers; k++) {
              if ((GET_LAYER(map, j)->name &&
                   strcasecmp(GET_LAYER(map, j)->name, layers[k]) == 0) ||
                  (GET_LAYER(map, j)->group &&
                   strcasecmp(GET_LAYER(map, j)->group, layers[k]) == 0)) {
                GET_LAYER(map, j)->status = MS_ON;
                break;
              }
            }
          }
        }

        msFreeCharArray(layers, num_layers);

        i += 1;
      }
    }

    image = msDrawMap(map, MS_FALSE);

    if (!image) {
      msWriteError(stderr);

      msFreeMap(map);
      msCleanup();
      msFreeConfig(config);
      free(mapfile_buffer);
      exit(1);
    }

    if (msSaveImage(map, image, outfile) != MS_SUCCESS) {
      msWriteError(stderr);
    }

    msFreeImage(image);
    msFreeMap(map);

    if (msGetGlobalDebugLevel() >= MS_DEBUGLEVEL_TUNING) {
      msGettimeofday(&requestendtime, NULL);
      msDebug("map2img total time: %.3fs\n",
              (requestendtime.tv_sec + requestendtime.tv_usec / 1.0e6) -
                  (requeststarttime.tv_sec + requeststarttime.tv_usec / 1.0e6));
    }

  } /*   for(draws=0; draws<iterations; draws++) { */
  msCleanup();
  msFreeConfig(config);
  free(mapfile_buffer);
  return (0);
} /* ---- END Main Routine ---- */
