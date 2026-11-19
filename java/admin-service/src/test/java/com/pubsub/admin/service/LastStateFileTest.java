package com.pubsub.admin.service;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class LastStateFileTest {

    @TempDir
    Path directory;

    /** The same line the framework's LastStateFile writes in C++, so that either can read the other's. */
    @Test
    void recordsTheTradingDayAndThePhaseInTheFrameworksFormat() throws Exception {
        String path = directory.resolve("admin_service.last_state").toString();
        LastStateFile.record(path, 20261119, LastStateFile.END_OF_DAY_END);
        assertEquals("trading_day=20261119 EndOfDayEnd\n", Files.readString(Path.of(path), StandardCharsets.US_ASCII));
        assertTrue(LastStateFile.says(path, 20261119, LastStateFile.END_OF_DAY_END));
    }

    @Test
    void theEndOfAnotherDayIsNotTheEndOfThisOne() throws Exception {
        String path = directory.resolve("admin_service.last_state").toString();
        LastStateFile.record(path, 20261118, LastStateFile.END_OF_DAY_END);
        assertFalse(LastStateFile.says(path, 20261119, LastStateFile.END_OF_DAY_END));
    }

    @Test
    void noFileOrNoPathSaysNothing() throws Exception {
        assertFalse(LastStateFile.says(directory.resolve("absent").toString(), 20261119, LastStateFile.END_OF_DAY_END));
        assertFalse(LastStateFile.says("", 20261119, LastStateFile.END_OF_DAY_END));
        LastStateFile.record("", 20261119, LastStateFile.END_OF_DAY_END);
    }
}
