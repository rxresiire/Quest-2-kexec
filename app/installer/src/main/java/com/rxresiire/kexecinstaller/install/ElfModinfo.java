package com.rxresiire.kexecinstaller.install;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Reads the {@code vermagic=} record out of an AArch64 module's {@code .modinfo}
 * section. Extracted on the host so a mismatched module is rejected before it is
 * ever copied to the headset.
 */
public final class ElfModinfo {

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
        if (sectionEntrySize != 64 || sectionCount == 0 || stringTableIndex >= sectionCount) {
            throw new IllegalArgumentException("Invalid ELF section table");
        }
        int tableStart = (int) sectionOffset;
        int namesStart = sectionFieldInt(elf, tableStart + stringTableIndex * 64, 16);
        int namesSize = sectionFieldInt(elf, tableStart + stringTableIndex * 64, 20);
        for (int index = 0; index < sectionCount; index++) {
            int entry = tableStart + index * 64;
            int nameOffset = sectionFieldInt(elf, entry, 0);
            String name = cString(elf, namesStart + nameOffset, namesStart + namesSize);
            if (!".modinfo".equals(name)) {
                continue;
            }
            int contentStart = sectionFieldInt(elf, entry, 16);
            int contentSize = sectionFieldInt(elf, entry, 20);
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

    private static int sectionFieldInt(byte[] elf, int entry, int fieldOffset) {
        return ByteBuffer.wrap(elf, entry + fieldOffset, 4)
                .order(ByteOrder.LITTLE_ENDIAN).getInt();
    }

    private static String cString(byte[] elf, int start, int limit) {
        int end = start;
        while (end < limit && elf[end] != 0) {
            end++;
        }
        return new String(elf, start, end - start, StandardCharsets.US_ASCII);
    }
}
