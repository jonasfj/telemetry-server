#!/usr/bin/env python
# encoding: utf-8

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import io
import sys
try:
    import simplejson as json
except ImportError:
    import json
from telemetry_schema import TelemetrySchema
import time
import logging


class StorageLayout:
    """A class for encapsulating the on-disk data layout for Telemetry"""
    COMPRESSED_SUFFIX = ".xz"
    if os.path.isfile("/usr/bin/xz"):
      COMPRESS_PATH = "/usr/bin/xz"
    elif os.path.isfile("/usr/local/bin/xz"):
      COMPRESS_PATH = "/usr/local/bin/xz"
    COMPRESSION_ARGS = ["-0"]
    DECOMPRESSION_ARGS = ["--decompress", "--stdout"]

    PENDING_COMPRESSION_SUFFIX = ".compressme"

    def __init__(self, schema, basedir, max_log_size):
        self._max_log_size = max_log_size
        self._schema = schema
        self._basedir = basedir

    def write(self, uuid, obj, dimensions, version=1):
        filename = self._schema.get_filename(self._basedir, dimensions, version)
        return self.write_filename(uuid, obj, filename)

    def clean_newlines(self, value, tag="value"):
        # Clean any newlines (replace with spaces)
        for eol in ["\r", "\n"]:
            if eol in value:
                logging.warn("Found an unexpected EOL in %s" % (tag))
                value = value.replace(eol, " ")
        return value

    def write_filename(self, uuid, obj, filename):
        # Working filename is like
        #   a.b.c.log
        # We want to roll this over (and compress) when it reaches a size limit

        # TODO: should we actually write "err" to file?
        if isinstance(obj, basestring):
            jsonstr = self.clean_newlines(unicode(obj), obj)
        else:
            # Use minimal json (without excess spaces)
            jsonstr = unicode(json.dumps(obj, separators=(',', ':')))

        output_line = u"%s\t%s\n" % (uuid, jsonstr)

        dirname = os.path.dirname(filename)
        if not os.path.exists(dirname):
            try:
                os.makedirs(dirname)
            except OSError, e:
                # errno 17 means "directory exists". This is a race condition
                # in a multi-process environment, and can safely be ignored.
                if e.errno != 17:
                    raise

        # According to SO, this should be atomic on a well-behaved OS:
        # http://stackoverflow.com/questions/7561663/appending-to-the-end-of-a-file-in-a-concurrent-environment
        with io.open(filename, "a") as fout:
            fout.write(output_line)
            filesize = fout.tell()

        logging.debug("Wrote to %s: new size is %d" % (filename, filesize))
        if filesize >= self._max_log_size:
            return self.rotate(filename)
        else:
            return filename

    def rotate(self, filename):
        logging.debug("Rotating %s" % (filename))

        # rename current file
        tmp_name = "%s.%d.%f%s" % (filename, os.getpid(), time.time(), self.PENDING_COMPRESSION_SUFFIX)
        os.rename(filename, tmp_name)
        # Note that files are expected to be compressed elsewhere
        # The compressed log filenames will be something like
        #   a.b.c.log.3.COMPRESSED_SUFFIX
        return tmp_name
