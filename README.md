# is-this-ascii

Checks that text contains only plain ASCII characters before you paste it
somewhere that matters.

Curly quotes, long dashes, non-breaking spaces, zero-width characters and
look-alike letters slip into text copied from documents, chat apps and web
pages. Most of them are invisible or hard to spot, and they can break code,
commands and configuration files or hide text from a reviewer.
is-this-ascii shows each one in red with its line and column, its name and
what to type instead. If everything is ASCII, it prints a green OK.

- Works offline: no network code. It only reads the clipboard, the console
  and the files you give it, and it changes nothing.
- One C file, no dependencies.

## What counts as ASCII here

- printable ASCII from 0x20 to 0x7E, including the space
- tab
- line breaks (LF or CRLF)

Everything else is reported, including control characters such as ESC and a
carriage return that is not followed by a line feed.

## Build

Windows, with MinGW GCC:

```
gcc -O2 -Wall -Wextra -o is-this-ascii.exe is-this-ascii.c
```

Linux (file checks only; the clipboard mode is Windows-only):

```
gcc -O2 -Wall -Wextra -o is-this-ascii is-this-ascii.c
```

## Usage

Check the clipboard (Windows):

1. Copy the text (Ctrl+C).
2. Run `is-this-ascii`. It checks the clipboard right away.
3. To check another text, copy it and press Enter. Type `q` and press Enter
   to quit.

Check files:

```
is-this-ascii notes.md report.txt
```

You can also drag a file onto `is-this-ascii.exe`.

Exit codes: 0 = all ASCII, 1 = problems found, 2 = the input could not be
read.

## Example

The program's messages are in Turkish.

```
$ is-this-ascii notes.txt
notes.txt: 1 satir, 29 karakter

Satir 1, sutun 9, 15, 17:
He said [U+201C]hello[U+201D] [U+2014] then left.

Bulunan karakterler:
  U+201C      1 kez   sol kivrik cift tirnak   yerine: "
  U+201D      1 kez   sag kivrik cift tirnak   yerine: "
  U+2014      1 kez   uzun tire (em dash)      yerine: -

SORUN - 3 karakter ASCII degil (1 satirda)
```

satir = line, sutun = column, kez = times, yerine = type instead,
SORUN = problem. A clean text gets `OK - tum karakterler ASCII`.

## Why the clipboard and not pasted text

The Windows console can change text while it is being pasted. Its
[FilterOnPaste](https://github.com/microsoft/terminal/blob/main/doc/ConsoleHostSettings.md)
setting, for example, replaces curly quotes with straight ones before the
program sees them. A checker that reads pasted text could then report OK for
text that still contains curly quotes. Reading the clipboard checks exactly
the text you are about to paste.

## Security notes

- Cyrillic and Greek letters that look like Latin letters (homoglyphs) are
  named together with the letter they imitate.
- Bidirectional control characters used in Trojan Source attacks are
  reported.
- Invisible Unicode tag characters are decoded, so text hidden with them
  becomes readable.
- Characters from the checked text are never printed raw. Control
  characters always appear as `[U+XXXX]`, so the checked text cannot affect
  your terminal.

ASCII characters that look alike, such as `rn` and `m` or `l`, `1` and `I`,
are not detected.

## License

MIT. See [LICENSE](LICENSE).
