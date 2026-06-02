package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.log.LogBuffer;
import io.javalin.http.Context;
import io.javalin.http.sse.SseClient;

import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

public class LogHandler {

    private final LogBuffer logBuffer;

    public LogHandler(LogBuffer logBuffer) {
        this.logBuffer = logBuffer;
    }

    public void getSnapshot(Context ctx) {
        ctx.json(logBuffer.snapshot());
    }

    public void stream(SseClient client) {
        List<String> snapshot = logBuffer.snapshot();
        for (String line : snapshot) {
            client.sendEvent("log", line);
        }

        BlockingQueue<String> queue = logBuffer.subscribe();
        try {
            while (!client.terminated()) {
                String line = queue.poll(2, TimeUnit.SECONDS);
                if (line != null) {
                    client.sendEvent("log", line);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            logBuffer.unsubscribe(queue);
        }
    }
}
