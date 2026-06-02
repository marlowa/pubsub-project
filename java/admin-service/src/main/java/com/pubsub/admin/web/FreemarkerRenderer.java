package com.pubsub.admin.web;

import freemarker.template.Configuration;
import freemarker.template.Template;
import io.javalin.http.Context;
import io.javalin.rendering.FileRenderer;

import java.io.IOException;
import java.io.StringWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.Map;

/** Adapts Freemarker to Javalin's FileRenderer interface. */
public class FreemarkerRenderer implements FileRenderer {
    private final Configuration cfg;
    private final String brandName;
    private final String brandLogoUrl;
    private final String brandCss;

    public FreemarkerRenderer(String brandName, String brandLogoUrl, String brandCssFile) {
        cfg = new Configuration(Configuration.VERSION_2_3_33);
        cfg.setClassForTemplateLoading(FreemarkerRenderer.class, "/");
        cfg.setDefaultEncoding("UTF-8");

        this.brandName = brandName;
        this.brandLogoUrl = brandLogoUrl;
        this.brandCss = loadBrandCss(brandCssFile);
    }

    @Override
    public String render(String filePath, Map<String, ? extends Object> model, Context context) {
        try {
            String name = filePath.startsWith("/") ? filePath.substring(1) : filePath;
            Template template = cfg.getTemplate(name);

            Map<String, Object> merged = new HashMap<>(model);
            merged.put("brandName", brandName);
            merged.put("brandLogoUrl", brandLogoUrl);
            merged.put("brandCss", brandCss);

            String username = context.sessionAttribute(AuthFilter.SESSION_USERNAME);
            if (username != null) {
                merged.put("currentUser", username);
                merged.put("isAdmin",
                        "ADMIN".equals(context.sessionAttribute(AuthFilter.SESSION_ROLE)));
            }

            StringWriter writer = new StringWriter();
            template.process(merged, writer);
            return writer.toString();
        } catch (Exception e) {
            throw new RuntimeException("Freemarker rendering failed for: " + filePath, e);
        }
    }

    private static String loadBrandCss(String brandCssFile) {
        if (brandCssFile == null || brandCssFile.isBlank()) {
            return "";
        }
        Path path = Paths.get(brandCssFile);
        if (!Files.exists(path)) {
            return "";
        }
        try {
            return Files.readString(path);
        } catch (IOException e) {
            return "";
        }
    }
}
