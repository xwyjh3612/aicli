package com.aitest.aicli;

import java.util.function.IntUnaryOperator;

/** Matches Windows Terminal's default word-selection character classes. */
final class TerminalWordBoundary {
    private static final String WORD_DELIMITERS =
            " /\\()\"'-.,:;<>~!@#$%^&*|+=[]{}~?\u2502";

    static final class Range {
        final int start;
        final int end;

        Range(int start, int end) {
            this.start = start;
            this.end = end;
        }
    }

    private TerminalWordBoundary() {}

    static Range find(int columns, IntUnaryOperator codePointAt, int touchedColumn) {
        if (columns <= 0) return new Range(0, 0);
        int column = normalizeColumn(columns, codePointAt, touchedColumn);
        int characterClass = characterClass(columns, codePointAt, column);
        int start = column;
        while (start > 0 && characterClass(columns, codePointAt, start - 1) == characterClass) {
            start--;
        }
        int end = column;
        while (end + 1 < columns &&
                characterClass(columns, codePointAt, end + 1) == characterClass) {
            end++;
        }
        return new Range(start, end);
    }

    static int normalizeColumn(int columns, IntUnaryOperator codePointAt, int touchedColumn) {
        if (columns <= 0) return 0;
        int column = clamp(touchedColumn, 0, columns - 1);
        while (column > 0 && codePointAt.applyAsInt(column) <= 0) column--;
        return column;
    }

    private static int characterClass(int columns, IntUnaryOperator codePointAt, int column) {
        int normalized = clamp(column, 0, Math.max(0, columns - 1));
        int codePoint = codePointAt.applyAsInt(normalized);
        while (codePoint <= 0 && normalized > 0) {
            codePoint = codePointAt.applyAsInt(--normalized);
        }
        if (codePoint <= ' ') return 0;
        return WORD_DELIMITERS.indexOf(codePoint) >= 0 ? 1 : 2;
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
