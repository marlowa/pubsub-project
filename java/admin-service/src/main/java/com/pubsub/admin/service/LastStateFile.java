package com.pubsub.admin.service;

import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.List;

/**
 * The administration service's last trading state: one line, {@code trading_day=20261116 EndOfDayEnd}, the same format
 * as the framework's LastStateFile in C++ (docs/venue/start_and_end_of_day.md, section 4.2, decision 5).
 *
 * <p>It is written to a temporary file that is flushed and then renamed over the real one, so that a reader sees the
 * previous line or the new one, never part of one. It outlives the day. The service reads it first when it starts: if it
 * says the end-of-day end of the day the service was started for, the service has no work to do, and exits at once for
 * the end of day.
 */
public final class LastStateFile {

    /** The name of the phase that ends the trading day, as the framework writes it. */
    public static final String END_OF_DAY_END = "EndOfDayEnd";

    private static final String TRADING_DAY_FIELD = "trading_day=";

    private LastStateFile() {
    }

    /**
     * Records that the phase named {@code phase} of {@code tradingDay} has been seen. A blank path records nothing.
     *
     * @throws IOException if the file cannot be written
     */
    public static void record(String path, int tradingDay, String phase) throws IOException {
        if (path == null || path.isBlank()) {
            return;
        }
        Path target = Path.of(path);
        Path temporary = Path.of(path + ".tmp");
        byte[] line = (TRADING_DAY_FIELD + tradingDay + " " + phase + "\n").getBytes(StandardCharsets.US_ASCII);
        try (FileChannel channel = FileChannel.open(temporary, StandardOpenOption.CREATE, StandardOpenOption.WRITE,
                StandardOpenOption.TRUNCATE_EXISTING)) {
            channel.write(java.nio.ByteBuffer.wrap(line));
            channel.force(true);
        }
        Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    /** Whether the file at {@code path} says the phase named {@code phase} of {@code tradingDay} has been seen. */
    public static boolean says(String path, int tradingDay, String phase) {
        if (path == null || path.isBlank()) {
            return false;
        }
        List<String> lines;
        try {
            lines = Files.readAllLines(Path.of(path), StandardCharsets.US_ASCII);
        } catch (IOException e) {
            return false;
        }
        return !lines.isEmpty() && lines.get(0).strip().equals(TRADING_DAY_FIELD + tradingDay + " " + phase);
    }
}
