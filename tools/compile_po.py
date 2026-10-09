#!/usr/bin/env python3
"""Compile a UTF-8 GNU gettext PO catalog to a GNU MO catalog."""

import ast
import shutil
import os
import re
import struct
import sys


def read_po(path):
    entries, current, active = [], {}, None

    def finish():
        if current:
            entries.append(dict(current))
            current.clear()

    with open(path, "r", encoding="utf-8-sig") as source:
        for raw in source:
            line = raw.strip()
            if not line:
                finish()
                active = None
                continue
            if line.startswith("#"):
                continue
            if line.startswith('msgctxt '):
                active = "msgctxt"
                current[active] = ast.literal_eval(line[len(active):].strip())
            elif line.startswith('msgid_plural '):
                active = "msgid_plural"
                current[active] = ast.literal_eval(line[len(active):].strip())
            elif line.startswith('msgid '):
                active = "msgid"
                current[active] = ast.literal_eval(line[len(active):].strip())
            elif line.startswith('msgstr['):
                end = line.index("]")
                active = "msgstr[{}]".format(int(line[7:end]))
                current[active] = ast.literal_eval(line[end + 1:].strip())
            elif line.startswith("msgstr "):
                active = "msgstr"
                current[active] = ast.literal_eval(line[len("msgstr"):].strip())
            elif line.startswith('"') and active is not None:
                current[active] += ast.literal_eval(line)
        finish()
    return entries


def compile_mo(entries, output):
    messages = {}
    header = next((entry.get("msgstr", "") for entry in entries
                   if entry.get("msgid", "") == ""), "")
    plural_match = re.search(r"nplurals\s*=\s*(\d+)", header)
    plural_count = int(plural_match.group(1)) if plural_match else 2
    for entry in entries:
        msgid = entry.get("msgid", "")
        context = entry.get("msgctxt")
        if context is not None:
            msgid = context + "\x04" + msgid
        if "msgid_plural" in entry:
            msgid += "\x00" + entry["msgid_plural"]
            forms = [entry.get("msgstr[{}]".format(i), "") for i in range(plural_count)]
            translated = "\x00".join(forms)
        else:
            translated = entry.get("msgstr", "")
        if translated:
            messages[msgid.encode("utf-8")] = translated.encode("utf-8")

    pairs = sorted(messages.items())
    count = len(pairs)
    original_table = 28
    translated_table = original_table + count * 8
    string_offset = translated_table + count * 8
    originals, translations = bytearray(), bytearray()
    original_index, translated_index = [], []
    for original, _ in pairs:
        original_index.append((len(original), string_offset + len(originals)))
        originals.extend(original + b"\0")
    translated_offset = string_offset + len(originals)
    for _, translated in pairs:
        translated_index.append((len(translated), translated_offset + len(translations)))
        translations.extend(translated + b"\0")

    with open(output, "wb") as mo:
        mo.write(struct.pack("<7I", 0x950412DE, 0, count, original_table,
                             translated_table, 0, 0))
        for length, offset in original_index:
            mo.write(struct.pack("<2I", length, offset))
        for length, offset in translated_index:
            mo.write(struct.pack("<2I", length, offset))
        mo.write(originals)
        mo.write(translations)


if __name__ == "__main__":
    output = os.path.abspath(sys.argv[2])
    os.makedirs(os.path.dirname(output), exist_ok=True)
    compile_mo(read_po(sys.argv[1]), output)
    # Keep Meson's in-tree catalog in wxWidgets' standard locale layout too.
    portable = os.path.join(os.path.dirname(output), "po", "zh_CN", "LC_MESSAGES", "aegisub.mo")
    if os.path.normcase(portable) != os.path.normcase(output):
        os.makedirs(os.path.dirname(portable), exist_ok=True)
        shutil.copyfile(output, portable)
