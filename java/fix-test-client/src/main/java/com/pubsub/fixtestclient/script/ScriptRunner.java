package com.pubsub.fixtestclient.script;

import com.pubsub.fixtestclient.capture.MessageCapture;
import groovy.lang.Binding;
import groovy.lang.GroovyShell;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.StringWriter;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;

public class ScriptRunner {

    private static final Logger log = LoggerFactory.getLogger(ScriptRunner.class);

    private final FixSessionBinding sessionBinding;
    private final FixHelper fixHelper;
    private final MessageCapture messageCapture;

    private final ExecutorService executor = Executors.newSingleThreadExecutor(
            runnable -> {
                Thread thread = new Thread(runnable, "script-runner");
                thread.setDaemon(true);
                return thread;
            });

    private final AtomicReference<ScriptState> state = new AtomicReference<>(ScriptState.IDLE);
    private volatile String output = "";
    private volatile String errorMessage = "";
    private volatile String scriptContent = "";
    private volatile Thread runningThread;

    public ScriptRunner(FixSessionBinding sessionBinding, FixHelper fixHelper,
                        MessageCapture messageCapture) {
        this.sessionBinding = sessionBinding;
        this.fixHelper = fixHelper;
        this.messageCapture = messageCapture;
    }

    public boolean run(String script) {
        if (!state.compareAndSet(ScriptState.IDLE, ScriptState.RUNNING)
                && !state.compareAndSet(ScriptState.COMPLETED, ScriptState.RUNNING)
                && !state.compareAndSet(ScriptState.FAILED, ScriptState.RUNNING)) {
            return false;
        }
        scriptContent = script;
        output = "";
        errorMessage = "";
        messageCapture.start();
        executor.submit(() -> executeScript(script));
        return true;
    }

    public void stop() {
        Thread thread = runningThread;
        if (thread != null) {
            thread.interrupt();
        }
        messageCapture.stop();
        state.compareAndSet(ScriptState.RUNNING, ScriptState.IDLE);
    }

    public ScriptState state() {
        return state.get();
    }

    public String output() {
        return output;
    }

    public String errorMessage() {
        return errorMessage;
    }

    public String scriptContent() {
        return scriptContent;
    }

    public void setScriptContent(String content) {
        this.scriptContent = content;
    }

    private void executeScript(String script) {
        runningThread = Thread.currentThread();
        StringWriter outputWriter = new StringWriter();

        groovy.lang.Closure<Void> sleepClosure = new groovy.lang.Closure<Void>(this) {
            @Override
            public Void call(Object... args) {
                long ms = ((Number) args[0]).longValue();
                try {
                    Thread.sleep(ms);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new RuntimeException("Script interrupted", e);
                }
                return null;
            }
        };

        Binding binding = new Binding();
        binding.setVariable("session", sessionBinding);
        binding.setVariable("fix", fixHelper);
        binding.setVariable("out", new java.io.PrintWriter(outputWriter, true));
        binding.setVariable("sleep", sleepClosure);

        try {
            GroovyShell shell = new GroovyShell(binding);
            shell.evaluate(script);
            output = outputWriter.toString();
            state.set(ScriptState.COMPLETED);
        } catch (Exception e) {
            output = outputWriter.toString();
            errorMessage = e.getClass().getSimpleName() + ": " + e.getMessage();
            log.warn("Script failed: {}", errorMessage);
            state.set(ScriptState.FAILED);
        } finally {
            messageCapture.stop();
            runningThread = null;
        }
    }
}
