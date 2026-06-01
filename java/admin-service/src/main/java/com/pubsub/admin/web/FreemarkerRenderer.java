package com.pubsub.admin.web;

import freemarker.template.Configuration;
import freemarker.template.Template;
import io.javalin.http.Context;
import io.javalin.rendering.FileRenderer;

import java.io.StringWriter;
import java.util.Map;

/** Adapts Freemarker to Javalin's FileRenderer interface. */
public class FreemarkerRenderer implements FileRenderer {
    private final Configuration cfg;

    public FreemarkerRenderer() {
        cfg = new Configuration(Configuration.VERSION_2_3_33);
        cfg.setClassForTemplateLoading(FreemarkerRenderer.class, "/");
        cfg.setDefaultEncoding("UTF-8");
    }

    @Override
    public String render(String filePath, Map<String, ? extends Object> model,
                         @SuppressWarnings("unused") Context context) {
        try {
            String name = filePath.startsWith("/") ? filePath.substring(1) : filePath;
            Template template = cfg.getTemplate(name);
            StringWriter writer = new StringWriter();
            template.process(model, writer);
            return writer.toString();
        } catch (Exception e) {
            throw new RuntimeException("Freemarker rendering failed for: " + filePath, e);
        }
    }
}
