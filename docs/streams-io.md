<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Streams and I/O in Trix

Trix provides a stream-based I/O system with automatic resource management,
configurable sandboxing, and full integration with error handling, save/restore,
and snap-shot/thaw.  Streams are the interface between Trix scripts and the
outside world: files, standard I/O, and in-memory buffers.

This document covers the complete I/O system: all operators, stream modes,
resource management patterns, file system operations, formatted I/O, and
configuration for embedded use.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Opening and Closing Streams](#3-opening-and-closing-streams)
4. [Reading Data](#4-reading-data)
5. [Writing Data](#5-writing-data)
6. [Resource Management with with-stream](#6-resource-management-with-with-stream)
7. [Standard Streams](#7-standard-streams)
8. [Stream Position and Seeking](#8-stream-position-and-seeking)
9. [File System Operations](#9-file-system-operations)
10. [Formatted I/O](#10-formatted-io)
11. [Memory Streams and String Assembly](#11-memory-streams-and-string-assembly)
12. [Script Loading](#12-script-loading)
13. [Binary I/O](#13-binary-io)
14. [Error Handling](#14-error-handling)
15. [Configuration and Sandboxing](#15-configuration-and-sandboxing)
16. [Save/Restore and Snap-shot/Thaw](#16-saverestore-and-snap-shotthaw)
17. [Real-World Patterns](#17-real-world-patterns)

---

## 1. Overview

A stream in Trix is a buffered connection to a file, a standard I/O channel,
or a memory buffer.  Streams are first-class Objects: they can be stored in
variables, passed to operators, and placed in collections.

Key characteristics:

- **Buffered I/O**: Each stream has a configurable buffer (default size set at
  construction). Reads and writes go through the buffer, reducing system calls.
- **Eight modes**: Read, write, append, exclusive create, plus read+write
  variants for each.
- **Automatic cleanup**: `with-stream` guarantees the stream is closed whether
  the body succeeds, fails, or calls `stop`.
- **Configurable pool**: Up to 255 simultaneously open streams (default 4).
- **Sandboxable**: Stream access can be disabled entirely or per-channel for
  embedded use.
- **Serializable**: Stream state (paths, positions, memory buffers) is
  captured by snap-shot and restored by thaw.

---

## 2. Quick Reference

### Stream I/O

| Operator          | Stack Effect                   | Description                    |
| ----------------- | ------------------------------ | ------------------------------ |
| `stream`          | `str mode -- stream`           | Open file                      |
| `read`            | `stream -- byte true \| false` | Read one byte                  |
| `write`           | `stream byte --`               | Write one byte                 |
| `write-string`    | `stream str --`                | Write string                   |
| `read-all`        | `stream -- str`                | Read entire remaining content  |
| `read-string`     | `stream str -- str bool`       | Read up to N bytes into buffer |
| `read-hex-string` | `stream str -- str bool`       | Read hex-encoded data          |
| `read-line`       | `stream str -- str bool`       | Read one line (LF or CRLF)     |
| `close-stream`    | `stream --`                    | Close stream                   |
| `flush-stream`    | `stream --`                    | Flush write buffer             |
| `reset-stream`    | `stream --`                    | Discard internal buffer        |
| `status`          | `stream -- bool`               | True if open                   |
| `bytes-available` | `stream -- int`                | Bytes remaining in buffer      |
| `current-stream`  | `-- stream`                    | Current input execution stream |

### stdout Convenience

| Operator | Stack Effect | Description                           |
| -------- | ------------ | ------------------------------------- |
| `print`  | `str --`     | Write string to stdout                |
| `=`      | `any --`     | Pop, print with newline               |
| `==`     | `any --`     | Pop, print detailed form with newline |
| `nl`     | `--`         | Print newline                         |
| `flush`  | `--`         | Flush stdout                          |

### Resource Management

| Operator      | Stack Effect                    | Description                    |
| ------------- | ------------------------------- | ------------------------------ |
| `with-stream` | `str mode proc --`              | Open, exec proc, auto-close    |
| `token`       | `str -- post any true \| false` | Tokenize one value from string |
| `run`         | `str --`                        | Execute script file            |
| `require`     | `str --`                        | Idempotent script load         |

### File System

| Operator           | Stack Effect      | Description         |
| ------------------ | ----------------- | ------------------- |
| `file-exists?`     | `str -- bool`     | Test if file exists |
| `file-size`        | `str -- long`     | File size in bytes  |
| `delete-file`      | `str --`          | Delete file         |
| `rename-file`      | `old new --`      | Rename file         |
| `filename-for-all` | `pattern proc --` | Glob and iterate    |

### Stream Position

| Operator              | Stack Effect     | Description      |
| --------------------- | ---------------- | ---------------- |
| `stream-position`     | `stream -- long` | Current position |
| `set-stream-position` | `stream long --` | Seek to position |

---

## 3. Opening and Closing Streams

### 3.1 Stream Modes

The `stream` operator opens a file with a mode specified as a byte:

```
(data.txt) (r)#b stream     % open for reading
(out.txt) (w)#b stream      % open for writing (creates/truncates)
```

Eight modes are available:

| Mode    | Byte           | Meaning         | Creates?              | Truncates?        |
| ------- | -------------- | --------------- | --------------------- | ----------------- |
| `(r)#b` | Read           | Read only       | No (error if missing) | No                |
| `(w)#b` | Write          | Write only      | Yes                   | Yes               |
| `(a)#b` | Append         | Append only     | Yes                   | No (seeks to end) |
| `(e)#b` | Exclusive      | Write only      | Yes (error if exists) | N/A               |
| `(R)#b` | Read+Write     | Read and write  | No (error if missing) | No                |
| `(W)#b` | Write+Read     | Read and write  | Yes                   | Yes               |
| `(A)#b` | Append+Read    | Read and append | Yes                   | No (seeks to end) |
| `(E)#b` | Exclusive+Read | Read and write  | Yes (error if exists) | N/A               |

### 3.2 Manual Open and Close

```
(data.txt) (r)#b stream % open file, push stream
dup read-all            % read entire content
exch close-stream       % close stream
```

Manual management requires explicitly closing every stream.  Errors between
open and close can leak the stream handle.  Prefer `with-stream` for all
file operations.

### 3.3 Stream Predicates

```
(data.txt) (r)#b stream
dup status           % => true (stream is open)
dup readable?        % => true
dup writable?        % => false (opened read-only)
close-stream
```

---

## 4. Reading Data

### 4.1 Read Entire File

`read-all` reads from the current position to EOF and returns a string:

```
(data.txt) (r)#b { read-all } with-stream
% => string containing entire file
```

### 4.2 Read by Line

`read-line` reads until a newline (LF or CRLF).  It requires a buffer string
that determines the maximum line length:

```
(log.txt) (r)#b {
    1024 string                   % allocate line buffer
    {
        2 dup-n read-line         % stream buffer -- stream buffer str bool
        { = } { pop exit } if-else
    } loop
    pop pop
} with-stream
```

`read-line` returns `str true` on success (the string contains the line
without the newline) or `str false` at EOF.  If the final line has no trailing
newline, that last line is returned with the `false` flag (the buffer holds the
text, but the EOF flag is set on the same call); the `{ pop exit }` loop above
discards it.  To keep a no-newline final line, check the buffer length before
exiting on `false`.

### 4.3 Read Fixed Bytes

`read-string` reads up to N bytes into a buffer string:

```
(data.bin) (r)#b {
    256 string                   % 256-byte buffer
    read-string                  % stream buffer -- str bool
    % str contains bytes read, bool is true if buffer was filled
} with-stream
```

### 4.4 Read Single Byte

`read` reads one byte, returning it as a Byte with `true`, or just
`false` at EOF.  (`=` prints a Byte as its character, so this loop
echoes the file's text; use `0 add` or a cast to work with the
numeric value.)

```
(data.txt) (r)#b {
    { dup read { = } { exit } if-else } loop
} with-stream
```

### 4.5 Read Hex-Encoded Data

`read-hex-string` reads hex digit pairs from the stream and decodes them
into bytes.  Whitespace between hex digits is ignored:

```
% File contains: 48 65 6C 6C 6F
(hex.txt) (r)#b {
    10 string read-hex-string    % => (Hello) false  (5 bytes decoded; the
                                 %   10-byte buffer was not filled, so false)
} with-stream
```

---

## 5. Writing Data

### 5.1 Write String

```
(output.txt) (w)#b {
    dup (Hello, world!) write-string
    (Second line) write-string
} with-stream
```

### 5.2 Write Single Byte

```
(output.bin) (w)#b {
    dup 72b write      % 'H'
    dup 105b write     % 'i'
} with-stream
```

### 5.3 Append to File

```
(log.txt) (a)#b {
    (New log entry\n) write-string
} with-stream
```

### 5.4 Flush

`flush-stream` forces the buffer contents to disk:

```
(output.txt) (w)#b {
    (important data) write-string
    dup flush-stream               % ensure data reaches disk
    % ... more operations ...
} with-stream
```

`flush` (no argument) flushes stdout:

```
(progress: ) print flush    % ensure output appears immediately
```

---

## 6. Resource Management with with-stream

### 6.1 Basic Usage

`with-stream` opens a file, pushes the stream onto the operand stack,
executes the proc, and guarantees the stream is closed afterward:

```
(data.txt) (r)#b { read-all } with-stream
% stream is closed; file contents on stack
```

### 6.2 Guaranteed Cleanup

The stream is closed on every exit path:

```
% Normal return: stream closed
(data.txt) (r)#b { read-all } with-stream

% Error: stream closed, then error propagates
{ (data.txt) (r)#b { /type-check throw } with-stream } try
% => /type-check  (stream was closed before error reached try)

% stop: stream closed
{ (data.txt) (r)#b { stop } with-stream } stopped
% => true (stream was closed)
```

### 6.3 Nested with-stream

Each `with-stream` manages its own stream independently:

```
(input.txt) (r)#b {
    read-all                          % read source
    (output.txt) (w)#b {
        exch write-string             % write to output
    } with-stream
} with-stream
% both streams closed
```

On error during the inner `with-stream`, both streams are closed (inner
first, then outer):

```
{
    (input.txt) (r)#b {
        (output.txt) (w)#b {
            /range-check throw        % error in inner body
        } with-stream
    } with-stream
} try
% => /range-check  (both streams closed)
```

### 6.4 Copy File

```
/copy-file {
    % src-path dst-path --
    exch
    (r)#b {
        read-all
        exch (w)#b {
            exch write-string
        } with-stream
    } with-stream
} def

(source.txt) (dest.txt) copy-file
```

### 6.5 Process File Line by Line

```
/process-lines {
    % filename proc -- (calls proc with each line)
    /handler exch def
    (r)#b {
        1024 string
        {
            2 dup-n read-line
            { handler } { pop exit } if-else
        } loop
        pop pop
    } with-stream
} def

(data.txt) { uppercase = } process-lines
```

---

## 7. Standard Streams

Four standard streams are available as built-in variables:

| Variable  | Description                    | Default |
| --------- | ------------------------------ | ------- |
| `stdin`   | Standard input (fd 0)          | Enabled |
| `stdout`  | Standard output (fd 1)         | Enabled |
| `stderr`  | Standard error (fd 2)          | Enabled |
| `stdedit` | Interactive editing (readline) | Enabled |

These four variables, plus the `=string` memory stream (SID 5) and the startup
stream (SID 6), occupy 6 of the stream-pool slots (see Section 15.1); the rest
are available for user file streams.

### 7.1 Reading from stdin

```
stdin read-line { (You typed: ) print = } if
```

### 7.2 Writing to stderr

```
stderr (Warning: something happened\n) write-string
```

### 7.3 Formatted Output to stderr

```
stderr ({}\n) mark 42 fprint-fmt pop pop
```

### 7.4 Convenience Operators

`print`, `=`, `==`, `nl`, and `flush` operate on stdout without requiring
the stream object:

```
(Hello) print nl % write "Hello\n" to stdout
42 =             % write "42\n" to stdout
flush            % flush stdout buffer
```

---

## 8. Stream Position and Seeking

Seekable streams (file streams, not stdin/stdout/pipes) support position
queries and seeking:

```
(data.txt) (R)#b {
    /s exch def
    s stream-position        % => 0l (start of file)
    s read-all length        % read everything, get length
    s 0l set-stream-position % seek back to start
    s read-all               % read again from beginning
} with-stream
% leaves on the stack: 0l <length> <full-contents>
```

`stream-position` returns a Long value.  `set-stream-position` takes a Long.

### 8.1 Random Access Pattern

```
(records.dat) (R)#b {
    /stream exch def
    /record-size 64 def

    % Read record N
    /read-record {
        record-size mul /long-type cast
        stream exch set-stream-position
        stream record-size string read-string pop
    } def

    5 read-record     % read 6th record (0-based)
} with-stream
```

---

## 9. File System Operations

### 9.1 File Existence and Size

```
(config.json) file-exists?         % => true or false
(config.json) file-size            % => long (bytes)
```

### 9.2 Delete and Rename

```
(temp.txt) delete-file           % delete file
(old.txt) (new.txt) rename-file  % rename file
```

### 9.3 Directory Listing (Glob)

`filename-for-all` iterates over files matching a glob pattern:

```
(*.txt) { = } filename-for-all    % print all .txt files

% Collect matching files into an array.  filename-for-all requires a
% `value --` proc (each iteration must consume the filename), so accumulate
% into a named array with `append` rather than a bare `{ }`.
[ ] /files exch def
(data/*.csv) { files exch append /files exch def } filename-for-all
files
```

### 9.4 Create Directory

```
(output) mkdir     % create directory
```

---

## 10. Formatted I/O

### 10.1 Printf-Style Output

Format strings use `{}` placeholders with optional index and format spec:

```
% The format string comes FIRST, then the mark and the arguments.
({} scored {} points\n) mark 42 (Alice) print-fmt pop pop
% prints: 42 scored Alice points    ({} fills in stack order: {}=42, {}=Alice)

% Indexed arguments
({1} scored {0} points\n) mark 42 (Alice) print-fmt pop pop
% prints: Alice scored 42 points    ({0} = 42, {1} = Alice)
```

### 10.2 Format to String

`sprint-fmt` writes into a destination string:

```
=string ({} + {} = {}) mark 3 4 7 sprint-fmt pop
% => (3 + 4 = 7)
```

### 10.3 Format to Stream

`fprint-fmt` writes to any stream:

```
(output.txt) (w)#b {
    % inside the block the stream is on the stack; fprint-fmt is
    % `stream fmt mark args... -- int bool`, so push fmt, mark, args above it.
    (Result: {}\n) mark 42 fprint-fmt pop pop
} with-stream
```

### 10.4 Array-Argument Variants

When arguments are in an array rather than on the stack:

```
({} scored {} points\n) [ (Alice) 42 ] aprint-fmt pop pop
```

### 10.5 Scan (Parse) from String

`sscan-fmt` parses structured input.  Its signature is
`input-str fmt-str mark args... -- parsed... count`: the input string and the
indexed format string come first, then a mark and one *type-template* argument
per placeholder (the template's type selects the parse target).  The parsed
values replace the templates and a count is pushed on top.

```
% Parse an integer and a real from whitespace-separated input.
(42 3.14) ({0} {1}) mark 0 0.0 sscan-fmt
% => 42 3.14 2     (the 0 template -> integer, the 0.0 template -> real,
%                   2 placeholders parsed)
```

### 10.6 Format Specifiers

```
{0}         % default format for argument 0
{0:d}       % integer decimal
{0:x}       % integer hexadecimal
{0:o}       % integer octal
{0:b}       % integer binary
{0:f}       % floating point
{0:e}       % scientific notation
{0:g}       % general (auto-select f or e)
{0:s}       % string
{0:c}       % character (byte)
{0:O}       % object detailed form (--type--)
{0:#s}      % object alt form (UPPERCASE type name)
```

Width and precision: `{0:10d}` (width 10), `{0:.2f}` (2 decimal places),
`{0:10.5f}` (width 10, precision 5).

Flags: `-` (left-align), `0` (zero-pad), `+` (show sign), ` ` (space for
positive).

---

## 11. Memory Streams and String Assembly

### 11.1 The =string Pattern

`=string` returns a reusable String buffer (the shared `STRING_SID = 5`
slot) suitable as the destination of a single `sprint-fmt`.  It is a
String value, not a Stream, so it is the standard *one-shot* way to build
a string from formatted data:

```
=string ({} items at {} each) mark 5 3.50 sprint-fmt pop
% => (5 items at 3.5 each)
```

### 11.2 Multi-Step String Building

To assemble a string incrementally across several writes, use a writable
memory stream from `make-string-stream N` (an actual Stream that accepts
`write-string`, `fprint-fmt`, etc.), then extract the accumulated bytes
with `get-string-stream`.  `=string` does not work here -- it is a String,
not a Stream, so `write-string` would raise `/type-check`:

```
64 make-string-stream
dup (Header: ) write-string
dup ({}) mark 42 fprint-fmt pop pop
dup (\n) write-string
get-string-stream
% result string is "Header: 42\n"
```

### 11.3 Memory Stream Characteristics

- Backed by VM heap allocation
- Persist within save/restore boundaries
- Serialized by snap-shot (restored on thaw)
- The `=string` buffer is a single shared slot (STRING_SID = 5); each
  `make-string-stream` call allocates an independent writable stream

---

## 12. Script Loading

### 12.1 run

`run` executes a script file.  Every call re-executes the file:

```
(library.trx) run     % execute library.trx
(library.trx) run     % execute again (re-runs everything)
```

### 12.2 require

`require` executes the file only once per canonical path.  Subsequent calls
for the same file are silently skipped:

```
(library.trx) require     % execute library.trx
(library.trx) require     % no-op (already loaded)
(./library.trx) require   % no-op (same canonical path via realpath)
```

The tracking dict participates in save/restore: entries added after a save
point are rolled back on restore, allowing re-require after restore.
Circular requires are safe (the entry is recorded before execution begins).

### 12.3 require-module

`require-module` combines `require` with module verification:

```
(math-utils.trx) /math-utils require-module
% loads file (if not already loaded) and verifies /math-utils module exists
```

### 12.4 token

`token` parses a single Trix value from a string:

```
(42 hello) token     % => ( hello) 42 true
% post = remaining string, any = parsed value, true = success

() token             % => () false (empty string, nothing to parse)
```

---

## 13. Binary I/O

### 13.1 Pack and Unpack

`pack` encodes values into a binary string.  `unpack` decodes them back:

```
% Pack: uint16 + int32 + double
mark 16#CAFE#u 42 3.14d (>HId) pack
% => binary string (big-endian: 2 + 4 + 8 = 14 bytes)

% Unpack
dup (>HId) unpack
% => 0xCAFE 42 3.14d 3  (3 values unpacked)
```

### 13.2 Format Specifiers

| Spec    | Type         | Size    |
| ------- | ------------ | ------- |
| `b`/`B` | int8/uint8   | 1 byte  |
| `h`/`H` | int16/uint16 | 2 bytes |
| `i`/`I` | int32/uint32 | 4 bytes |
| `l`/`L` | int64/uint64 | 8 bytes |
| `f`     | float32      | 4 bytes |
| `d`     | float64      | 8 bytes |
| `x`     | padding      | 1 byte  |
| `Ns`    | string       | N bytes |

Endianness prefixes: `>` big-endian, `<` little-endian, `=` native (default).
Prefixes are sticky (apply to all subsequent specifiers).

Repeat counts: `4B` = four uint8 values.  `3x` = 3 padding bytes.

### 13.3 pack-size

`pack-size` returns the byte count for a format without packing:

```
(>HId) pack-size     % => 14 (2 + 4 + 8)
```

### 13.4 Binary Token Serialization

`to-binary-token` serializes any Object to compact binary format:

```
42 to-binary-token           % => binary string encoding integer 42
3.14d to-binary-token        % => binary string encoding double 3.14
```

### 13.5 Reading Binary Data

Combine `read-string` with `unpack` for binary file processing:

```
(data.bin) (r)#b {
    /stream exch def

    % Read a 4-byte header
    stream 4 string read-string pop
    (>I) unpack pop           % big-endian uint32

    % Read N records of 12 bytes each
    /count exch def
    count {
        stream 12 string read-string pop
        (>Ifd) unpack pop     % uint32 + float + double per record
    } repeat
} with-stream
```

### 13.6 Compression (deflate / inflate)

Trix uses zlib (a default runtime dependency, alongside `libreadline`).  Two pairs of operators expose it.  All four produce and
consume *raw* RFC 1951 DEFLATE bitstreams -- no zlib or gzip wrapper, no
header, no checksum trailer (build those yourself with `adler32` / `crc32`
and `pack` if you need RFC 1950 / RFC 1952 framing).  In a `TRIX_NO_ZLIB`
build (`build.sh --no-zlib`) all six deflate/inflate operators stay
registered but raise `/unsupported`; `crc32` / `adler32` are hand-rolled and
unaffected.

**In-memory (string in, string out).**  Use these when the whole input and
the whole output each fit in a Trix string (<= 65 535 bytes):

| Operator        | Stack effect     | Notes                                |
| --------------- | ---------------- | ------------------------------------ |
| `deflate`       | `str -- str`     | compress at default level 6          |
| `deflate-level` | `str int -- str` | compress at an explicit level `0..9` |
| `inflate`       | `str -- str`     | decompress a raw DEFLATE string      |

```
/payload (Hello, Hello, Hello, compression world!) def
payload deflate            % => compressed string (here 29 bytes)
dup inflate payload eq     % => true  (round-trips)
```

`deflate-level` lets you trade ratio for speed -- `0` stores with no
compression, `9` is maximum:

```
/data (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa) def
data 0 deflate-level length =   % => 53  (stored, slightly larger)
data 9 deflate-level length =   % => 6   (highly redundant input)
```

A level outside `0..9` raises `range-check`; feeding `inflate` a malformed,
truncated, or empty string raises `range-check`; an `inflate` whose output
overruns the available VM scratch raises `vm-full`.

**Streaming (stream in, stream out).**  Use these when the payload is too
large for one Trix string -- gzipping a multi-megabyte file, inflating a
long HTTP body.  zlib runs incrementally, so the working set never exceeds
two small scratch buffers regardless of total size:

| Operator               | Stack effect                  | Notes                       |
| ---------------------- | ----------------------------- | --------------------------- |
| `deflate-stream`       | `in-stream out-stream --`     | compress at default level 6 |
| `deflate-stream-level` | `in-stream out-stream int --` | compress at level `0..9`    |
| `inflate-stream`       | `in-stream out-stream --`     | decompress                  |

The input must be a *readable* stream and the output a *writable* one;
otherwise the op raises `invalid-stream-access`.  A `make-string-stream`
is write-only, so to feed an in-memory Trix string to a streaming op wrap
it as a read-only stream with `make-memory-stream` (it borrows the string's
bytes, no copy):

```
/payload (The quick brown fox jumps over the lazy dog. ) def

/in   payload make-memory-stream def     % read-only view of the string
/comp 65000 make-string-stream def       % writable destination
in comp deflate-stream
in close-stream
/comp-bytes comp get-string-stream def
comp close-stream                        % comp-bytes holds raw DEFLATE output

/cin comp-bytes make-memory-stream def   % feed the compressed bytes back
/out 65000 make-string-stream def
cin out inflate-stream
cin close-stream
/round out get-string-stream def
out close-stream
round payload eq =                       % => true
```

For file-backed work, open the source with `(r)#b` and the destination
with `(w)#b` and hand the two streams straight to `deflate-stream` --
nothing has to materialize in a Trix string.

---

## 14. Error Handling

### 14.1 I/O Error Codes

| Error                    | Cause                                      |
| ------------------------ | ------------------------------------------ |
| `/file-open-error`       | Permission denied, ENOTDIR, etc.           |
| `/filename-not-found`    | File does not exist (read modes)           |
| `/filename-exists`       | File already exists (exclusive modes)      |
| `/io-read-error`         | OS read() failed                           |
| `/io-write-error`        | OS write() failed                          |
| `/io-seek-error`         | lseek() failed                             |
| `/invalid-stream`        | Operation on a closed stream               |
| `/invalid-stream-access` | Read from write-only or write to read-only |

### 14.2 Error Handling Patterns

```
% Catch specific I/O errors
<< /filename-not-found { pop (File not found) }
   /file-open-error    { pop (Cannot open file) }
>>
{ (data.txt) (r)#b { read-all } with-stream }
try-catch

% Try-result for railway-style I/O
{ (config.json) (r)#b { read-all } with-stream } try-result
/ok { { parse-config } try-result } tag-bind
/ok 0 tag-value-or
```

### 14.3 with-stream + Error Propagation

`with-stream` closes the stream before the error propagates.  The error
reaches the enclosing `try` or `try-catch` with a clean resource state:

```
{
    (data.txt) (r)#b {
        read-all
        process-data         % may raise /type-check
    } with-stream
} try
% stream is closed regardless of whether process-data succeeded
```

---

## 15. Configuration and Sandboxing

### 15.1 Stream Pool Size

The maximum number of simultaneously open streams is configurable at
construction:

```
--stream-count=N         % 0..255, default 4
```

This controls the stream pool size.  Standard streams (stdin, stdout, stderr,
stdedit, =string, startup) consume 6 slots.  The remaining slots are
available for user file streams.

### 15.2 Buffer Size

Each stream's I/O buffer size is configurable:

```
--stream-buffer=BYTES    % 4..256K, default 4K
```

### 15.3 Disabling Streams (Sandboxing)

For embedded use where file access should be restricted:

```
--stream-io=none                 % disable all streams
--stream-io=stdout,stderr        % enable only output
```

With streams disabled, any attempt to open a file, read stdin, or use
`with-stream` raises an error.  This is the primary sandboxing mechanism
for untrusted scripts.

---

## 16. Save/Restore and Snap-shot/Thaw

### 16.1 Save/Restore

Streams opened after a save point are tracked.  On `restore`, those streams
are closed automatically:

```
save
    (temp.txt) (w)#b { (data) write-string } with-stream
restore
% stream was closed by with-stream; file persists on disk
```

The `require` tracking dict is rolled back on restore, allowing files to be
re-required after restore.

### 16.2 Snap-shot/Thaw

Snap-shot captures the state of all open streams:

- **Seekable file streams**: Path, mode, and position are serialized.  On thaw,
  files are re-opened at the saved position.
- **Memory streams**: Buffer contents are serialized with CRC-32 verification.
  On thaw, buffers are restored exactly.
- **Standard I/O**: Reconnected to file descriptors on thaw.
- **Non-seekable streams** (pipes): Closed before snap-shot.

This means a snap-shot image can be thawed on a different run and file
streams will reconnect to the same paths (if the files still exist).

---

## 17. Real-World Patterns

### 17.1 CSV Processing

```
/parse-csv-line {
    % str -- arr
    (,) split
    { trim } map
} def

/process-csv {
    % filename proc -- (calls proc with each row as array)
    /handler exch def
    (r)#b {
        1024 string
        {
            2 dup-n read-line
            { parse-csv-line handler } { pop exit } if-else
        } loop
        pop pop
    } with-stream
} def

(data.csv) { dup 0 get (Name: ) print = } process-csv
```

### 17.2 Configuration File

```
/load-config {
    % filename -- dict
    % Read the whole file, then split into lines.  (Reading line-by-line into
    % one reused buffer would alias the stored substrings, so read-all first.)
    /acc << >> def
    (r)#b { read-all } with-stream
    (\n) split {
        dup length 0 eq { pop } {
        dup (#) starts-with? { pop } {
            (=) split
            dup length 2 ge {
                dup 0 get trim          % key
                exch 1 get trim         % value
                acc 3 1 roll put        % acc key value put
            } { pop } if-else
        } if-else
        } if-else
    } for-all
    acc
} def

(app.conf) load-config
```

### 17.3 Log File with Timestamps

```
/log {
    % message --
    |msg|
    (app.log) (a)#b {
        % stream is on the stack; fprint-fmt is `stream fmt mark args... --`
        ([{}] {}\n) mark epoch-time msg fprint-fmt pop pop
    } with-stream
} def

(Application started) log
(Processing complete) log
```

### 17.4 Temp File with Cleanup

```
/with-temp-file {
    % proc -- result
    % proc receives: stream filename
    /body exch def
    /tmp-path (/tmp/trix-temp.dat) def
    tmp-path (w)#b {
        tmp-path body
    } with-stream
    tmp-path delete-file
} def
```

### 17.5 File Comparison

```
/files-equal? {
    % path1 path2 -- bool
    (r)#b { read-all } with-stream
    exch
    (r)#b { read-all } with-stream
    eq
} def

(file1.txt) (file2.txt) files-equal?
```

### 17.6 Batch File Processing

```
/process-all-txt {
    % proc -- (applies proc to contents of each .txt file)
    /handler exch def
    (*.txt) {
        (r)#b { read-all handler } with-stream
    } filename-for-all
} def

{ length = } process-all-txt    % print byte count of each .txt file
```

### 17.7 Binary Record Processing

```
/read-records {
    % filename record-fmt -- array-of-arrays
    /fmt exch def
    /record-size fmt pack-size def
    [ exch (r)#b {
        /stream exch def
        record-size string /buf exch def
        {
            stream buf read-string
            { mark exch fmt unpack pop array-from-mark }
            { pop exit }
            if-else
        } loop
    } with-stream ]
} def
```

---

## 18. Implementation Details

This section covers the C++ internals of the stream system for Trix
maintainers and contributors.

### 18.1 Stream Object Layout

Each stream is a C++ `Stream` object containing:

```
Stream {
    vm_offset_t m_base_offset;         // start of I/O buffer in VM heap
    vm_offset_t m_rlimit_offset;       // end of read data in buffer
    vm_offset_t m_rptr_offset;         // next byte to read
    vm_offset_t m_wlimit_offset;       // end of write buffer capacity
    vm_offset_t m_wptr_offset;         // next byte to write
    vm_offset_t m_next_offset;         // linked list chain (nulloffset = end)
    vm_t       *m_ext_base;            // external malloc buffer (memory/readline)
    vm_t       *m_ext_ptr;             // read position in external buffer
    size_t      m_ext_remaining;       // bytes remaining in external buffer
    Object      m_source;              // filename or source description
    int         m_fd;                  // OS file descriptor
    stream_buffer_size_t m_buffer_size; // buffer capacity
    status_t    m_status;              // 16-bit status flags
    uint16_t    m_line_pos;            // column (for scanner source locations)
    uint32_t    m_line_number;         // line number (for scanner)
    stream_id_t m_sid;                 // unique stream ID
    save_level_t m_stream_save_level;  // save level at open time
    vm_t        m_buffer[9];           // first 9 bytes inline; rest in VM heap
};
```

### 18.2 Status Flags

The `m_status` field is a 16-bit bitmask:

```
Bit  Flag                  Value   Meaning
---  --------------------  ------  ----------------------------------------
 --  IsClosed              0x0000  Stream is not open (no flags set)
  0  IsOpen                0x0001  Stream is open and usable
  1  IsReadable            0x0002  Read operations permitted
  2  IsWritable            0x0004  Write operations permitted
  3  IsStdIO               0x0008  stdin/stdout/stderr/stdedit
  4  IsFile                0x0010  Backed by a file descriptor
  5  IsStartupFile         0x0020  The initial script file
  6  IsString              0x0040  The =string memory stream (SID 5)
  7  IsMemory              0x0080  Backed by malloc'd external buffer
  8  SupportsRandomAccess  0x0100  Seekable (regular file, not pipe)
  9  ReadData              0x0200  Last operation was a read
 10  WriteData             0x0400  Last operation was a write
 11  IsAppend              0x0800  Opened in append mode
 12  IsStringWrite         0x1000  Writable in-memory string-stream
```

`ReadData`/`WriteData` enforce the POSIX requirement that switching between
reading and writing on a read+write stream requires an intervening seek or
flush.

### 18.3 Stream Pool

Streams are managed in two singly-linked lists via `m_next_offset`:

- **Free list** (`m_stream_free_list`): available stream slots
- **Inuse list** (`m_stream_inuse_list`): currently open streams

`alloc_stream()` pops from the free list and prepends to the inuse list.
`free_stream()` removes from the inuse list and prepends to the free list.
The pool is pre-allocated at construction with `m_stream_count` slots.

Each stream has a unique `stream_id_t` (`m_sid`). IDs are assigned
sequentially with wrap-around detection to prevent stale Object aliases.

### 18.4 Reserved Stream IDs

```
INVALID_SID  = 0    sentinel: no stream
STDIN_SID    = 1    standard input
STDEDIT_SID  = 2    readline interactive input
STDOUT_SID   = 3    standard output
STDERR_SID   = 4    standard error
STRING_SID   = 5    =string memory stream
STARTUP_SID  = 6    initial script file
FIRST_SID    = 7    first user-allocated stream
```

### 18.5 Buffer Management

Each stream has a single buffer in VM heap memory. The buffer serves as
either a read buffer or a write buffer depending on current direction:

**Read path:** `m_base_offset` to `m_rlimit_offset` contains data read
from the OS. `m_rptr_offset` is the next byte to return. When
`m_rptr_offset == m_rlimit_offset`, the buffer is empty and `read()` refills
it.

**Write path:** `m_base_offset` to `m_wlimit_offset` is buffer capacity.
`m_wptr_offset` is the next byte to write. When `m_wptr_offset ==
m_wlimit_offset`, the buffer is full and `flush()` writes it via OS
`write()`.

On a fresh open, the write pointer is initialized at the limit to force
a flush on the first write (which resets the buffer).

### 18.6 Mode Byte to OS Open Flags

```
Mode   Status Flags                        OS Flags
----   --------------------------------    ----------------------------
'r'    IsReadable                          O_RDONLY
'w'    IsWritable                          O_CREAT | O_WRONLY | O_TRUNC
'a'    IsWritable | IsAppend               O_CREAT | O_WRONLY
'e'    IsWritable                          O_CREAT | O_EXCL | O_WRONLY
'R'    IsReadable | IsWritable             O_RDWR
'W'    IsReadable | IsWritable             O_CREAT | O_RDWR | O_TRUNC
'A'    IsReadable | IsWritable | IsAppend  O_CREAT | O_RDWR
'E'    IsReadable | IsWritable             O_CREAT | O_EXCL | O_RDWR
```

### 18.7 Memory Streams

The `=string` stream (SID 5) uses `m_ext_base` (a malloc'd buffer) instead
of a file descriptor. Writes go to the external buffer; the result is
captured as a Trix string when the formatting operation completes.

Memory streams are serialized by snap-shot (buffer contents with CRC-32)
and restored by thaw (buffer re-allocated, contents copied back).
