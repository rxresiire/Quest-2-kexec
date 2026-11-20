package com.rxresiire.kexecinstaller.install;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Reads the {@code vermagic=} record out of an AArch64 module's {@code .modinfo}
 * section. Extracted on the host so a mismatched module is rejected before it is
 * ever copied to the headset.
 *
 * <p>ELF64 layouts used here: {@code e_shoff} is a 64-bit field at byte 40,
 * {@code e_shentsize}/{@code e_shnum}/{@code e_shstrndx} are 16-bit fields at
 * bytes 58/60/62, {@code sh_name} is a 32-bit field at byte 0 of a section
 * header, and {@code sh_offset}/{@code sh_size} are 64-bit fields at bytes
 * 24/32.
 */
public final class ElfModinfo {

    private static final int SECTION_HEADER_SIZE = 64;

    private ElfModinfo() {
    }

    public static String vermagic(byte[] elf) {
        if (elf.length < 64 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L'
                || elf[3] != 'F' || elf[4] != 2 || elf[5] != 1) {
            throw new IllegalArgumentException("Not an ELF64 little-endian file");
        }
        ByteBuffer buffer = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        long sectionOffset = buffer.getLong(40);
        int sectionEntrySize = buffer.getShort(58) & 0xffff;
        int sectionCount = buffer.getShort(60) & 0xffff;
        int stringTableIndex = buffer.getShort(62) & 0xffff;
        if (sectionEntrySize != SECTION_HEADER_SIZE || sectionCount == 0
                || stringTableIndex >= sectionCount) {
            throw new IllegalArgumentException("Invalid ELF section table");
        }
        if (sectionOffset < 0
                || sectionOffset + (long) sectionCount * SECTION_HEADER_SIZE > elf.length) {
            throw new IllegalArgumentException("ELF section table is outside the file");
        }
        int tableStart = (int) sectionOffset;
        int namesStart = (int) sectionFieldLong(elf, tableStart + stringTableIndex * SECTION_HEADER_SIZE, 24);
        int namesSize = (int) sectionFieldLong(elf, tableStart + stringTableIndex * SECTION_HEADER_SIZE, 32);
        checkRange(elf, namesStart, namesSize, "section name table");
        int namesLimit = namesStart + namesSize;
        for (int index = 0; index < sectionCount; index++) {
            int entry = tableStart + index * SECTION_HEADER_SIZE;
            int nameOffset = sectionFieldInt(elf, entry, 0);
            if (nameOffset < 0 || namesStart + nameOffset >= namesLimit) {
                continue;
            }
            String name = cString(elf, namesStart + nameOffset, namesLimit);
            if (!".modinfo".equals(name)) {
                continue;
            }
            int contentStart = (int) sectionFieldLong(elf, entry, 24);
            int contentSize = (int) sectionFieldLong(elf, entry, 32);
            checkRange(elf, contentStart, contentSize, ".modinfo contents");
            String contents = new String(elf, contentStart, contentSize, StandardCharsets.US_ASCII);
            for (String record : contents.split("\0")) {
                if (record.startsWith("vermagic=")) {
                    return record.substring("vermagic=".length()).trim();
                }
            }
            throw new IllegalArgumentException(".modinfo has no vermagic record");
        }
        throw new IllegalArgumentException("Module has no .modinfo section");
    }

    private static void checkRange(byte[] elf, int start, int size, String what) {
        if (start < 0 || size < 0 || start + (long) size > elf.length) {
            throw new IllegalArgumentException(what + " is outside the file");
        }
    }

    private static int sectionFieldInt(byte[] elf, int entry, int fieldOffset) {
        return ByteBuffer.wrap(elf, entry + fieldOffset, 4)
                .order(ByteOrder.LITTLE_ENDIAN).getInt();
    }

    private static long sectionFieldLong(byte[] elf, int entry, int fieldOffset) {
        return ByteBuffer.wrap(elf, entry + fieldOffset, 8)
                .order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    private static String cString(byte[] elf, int start, int limit) {
        int end = start;
        while (end < limit && elf[end] != 0) {
            end++;
        }
        return new String(elf, start, end - start, StandardCharsets.US_ASCII);
    }
}
