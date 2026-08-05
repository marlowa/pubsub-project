package com.pubsub.admin.web;

import com.pubsub.admin.model.AdminRole;
import com.pubsub.admin.model.AdminUser;
import com.pubsub.admin.model.CompIdRow;
import com.pubsub.admin.model.FirmRow;
import com.pubsub.admin.model.GatewayPermissionRow;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.MethodSource;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.OffsetDateTime;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.stream.Collectors;
import java.util.stream.Stream;

import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Renders every Freemarker template against a representative model.
 *
 * This exists because of a bug that reached main and sat there: users/form.ftl opened with
 * {@code <@layout.page title="${user??'Edit User':'New User'}">}, but Freemarker has no ?:
 * ternary operator, so the template failed to parse. Freemarker parses a template as a whole
 * before executing any of it, which means a syntax error anywhere in one makes the entire
 * page fail -- and nothing else catches it. Checkstyle, SpotBugs and JaCoCo do not read .ftl
 * files, and no other test renders a template. GET /admin/users/new and
 * GET /admin/users/{name}/edit returned HTTP 500 from the day the template was written.
 *
 * Two properties are checked. Every template renders without throwing, in each of its
 * branches -- create and edit, empty and populated list, with and without an error message,
 * signed in and not. And every template in the source tree appears in the case list, so adding
 * a template without a case here fails the build rather than going unrendered.
 */
class FreemarkerTemplateRenderingTest {

    /** Rendered directly by no handler: layout.ftl is a macro library pulled in by #import. */
    private static final Set<String> MACRO_ONLY_TEMPLATES = Set.of("layout.ftl");

    private static final OffsetDateTime TIMESTAMP = OffsetDateTime.parse("2026-07-29T12:00:00Z");

    private static final FirmRow FIRM =
            new FirmRow("ACME", "Acme Trading Ltd", true, TIMESTAMP, TIMESTAMP);

    private static final CompIdRow COMP_ID = new CompIdRow(
            "ACME_TRADER1", "ACME",
            "0".repeat(64), "1".repeat(64), "2".repeat(32), 4096,
            true, false, 0, false, null,
            null, TIMESTAMP, TIMESTAMP, TIMESTAMP, TIMESTAMP,
            // No per-comp-id grace period: the gateway's own default applies. Renders as an
            // empty field, which is the case a ?c on a null would have thrown on.
            true, null,
            // Not pinned to any gateway instance either -- the same null-renders-empty case
            // for the two provisioning fields, which is what an unprovisioned member has.
            null, null);

    /** A locked row: exercises the nullable lockedReason and lockedAt being populated. */
    private static final CompIdRow LOCKED_COMP_ID = new CompIdRow(
            "ACME_TRADER2", "ACME",
            "0".repeat(64), "1".repeat(64), "2".repeat(32), 4096,
            false, true, 3, true, "Too many failed logins",
            TIMESTAMP, TIMESTAMP, TIMESTAMP, TIMESTAMP, TIMESTAMP,
            // An explicitly provisioned grace period, so the other rendering path is
            // covered too: cancelling enabled with a member-specific 60s window.
            true, 60,
            // Pinned to instance 1 with 2 as its backup, covering the populated path for
            // both provisioning fields.
            1, 2);

    private static final GatewayPermissionRow PERMISSION =
            new GatewayPermissionRow("ACME_TRADER1", "order", true, TIMESTAMP);

    private static final AdminUser ADMIN_USER =
            new AdminUser("alice", "$2a$12$hash", AdminRole.ADMIN, false);

    private static final AdminUser VIEWER_USER =
            new AdminUser("bob", "$2a$12$hash", AdminRole.VIEWER, true);

    /** One template rendered with one model. The name is what a failure reports. */
    private record RenderCase(String name, String templatePath, Map<String, Object> model) {
        @Override
        public String toString() {
            return name;
        }
    }

    private static RenderCase renderCase(String name, String template, Object... modelPairs) {
        Map<String, Object> model = new HashMap<>();
        for (int index = 0; index < modelPairs.length; index += 2) {
            model.put((String) modelPairs[index], modelPairs[index + 1]);
        }
        return new RenderCase(name, "/templates/" + template, model);
    }

    private static Stream<RenderCase> renderCases() {
        return Stream.of(
                // Unauthenticated entry points, which carry their own <head>.
                renderCase("login", "login.ftl"),
                renderCase("login with error", "login.ftl",
                        "error", "Invalid username or password."),
                renderCase("setup", "setup.ftl"),
                renderCase("setup with error", "setup.ftl",
                        "error", "Passwords do not match.", "username", "alice"),

                // Rendered inside layout.ftl, so these also exercise the nav bar. The signed-in
                // variants drive the <#if currentUser??> and <#if isAdmin> branches of the nav.
                renderCase("change password", "change-password.ftl"),
                renderCase("change password with error", "change-password.ftl",
                        "error", "Current password is incorrect."),
                renderCase("error page", "error.ftl", "message", "CompID not found: NOPE"),

                renderCase("firms list", "firms/list.ftl", "firms", List.of(FIRM)),
                renderCase("firms list empty", "firms/list.ftl", "firms", List.of()),
                renderCase("firms list as admin", "firms/list.ftl",
                        "firms", List.of(FIRM), "currentUser", "alice", "isAdmin", true),
                renderCase("firms list as viewer", "firms/list.ftl",
                        "firms", List.of(FIRM), "currentUser", "bob", "isAdmin", false),

                renderCase("firm new", "firms/form.ftl"),
                renderCase("firm edit", "firms/form.ftl",
                        "firm", FIRM, "compIds", List.of(COMP_ID, LOCKED_COMP_ID)),
                renderCase("firm edit with no comp ids", "firms/form.ftl",
                        "firm", FIRM, "compIds", List.of()),

                renderCase("comp ids list", "comp-ids/list.ftl",
                        "compIds", List.of(COMP_ID, LOCKED_COMP_ID)),
                renderCase("comp ids list for firm", "comp-ids/list.ftl",
                        "compIds", List.of(COMP_ID), "firmId", "ACME"),
                renderCase("comp ids list empty", "comp-ids/list.ftl", "compIds", List.of()),

                renderCase("comp id new", "comp-ids/form.ftl", "firmId", "ACME"),
                renderCase("comp id edit", "comp-ids/form.ftl", "row", COMP_ID),
                renderCase("comp id edit locked", "comp-ids/form.ftl", "row", LOCKED_COMP_ID),
                renderCase("comp id set password", "comp-ids/set-password.ftl",
                        "compId", "ACME_TRADER1"),

                renderCase("gateway permissions", "gateway-permissions/list.ftl",
                        "compId", "ACME_TRADER1", "permissions", List.of(PERMISSION)),
                renderCase("gateway permissions empty", "gateway-permissions/list.ftl",
                        "compId", "ACME_TRADER1", "permissions", List.of()),

                renderCase("users list", "users/list.ftl",
                        "users", List.of(ADMIN_USER, VIEWER_USER)),
                renderCase("users list empty", "users/list.ftl", "users", List.of()),
                renderCase("users list with error", "users/list.ftl",
                        "users", List.of(ADMIN_USER), "error", "Cannot delete the last ADMIN user."),

                // The two cases that were returning HTTP 500.
                renderCase("user new", "users/form.ftl"),
                renderCase("user edit", "users/form.ftl", "user", VIEWER_USER),
                renderCase("user edit with error", "users/form.ftl",
                        "user", ADMIN_USER, "error", "Cannot demote the last ADMIN user."),

                renderCase("user reset password", "users/reset-password.ftl",
                        "username", "bob"),
                renderCase("user reset password with error", "users/reset-password.ftl",
                        "username", "bob", "error", "Password must be at least 8 characters."));
    }

    private static FreemarkerRenderer renderer() {
        return new FreemarkerRenderer("PubSub Admin", "", "");
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("renderCases")
    void templateRenders(RenderCase testCase) {
        // renderTemplate throws RuntimeException on a parse or process failure, so an
        // unhandled escape is the failure -- no assertion needed for that part.
        String html = renderer().renderTemplate(testCase.templatePath(), testCase.model());

        assertTrue(html.contains("</html>"),
                () -> testCase.name() + " produced no complete document:\n" + html);
        assertTrue(html.contains("PubSub Admin"),
                () -> testCase.name() + " did not apply the brand name");
    }

    /**
     * Guards the case list above against drift. A template added without a case here would
     * otherwise never be rendered by any test, which is how the users/form.ftl bug survived.
     */
    @Test
    void everyTemplateHasARenderCase() throws IOException {
        Set<String> onDisk = new TreeSet<>(templateFilesInSourceTree());
        Set<String> covered = new TreeSet<>(renderCases()
                .map(testCase -> testCase.templatePath().substring("/templates/".length()))
                .collect(Collectors.toSet()));

        onDisk.removeAll(MACRO_ONLY_TEMPLATES);

        Set<String> uncovered = new TreeSet<>(onDisk);
        uncovered.removeAll(covered);
        assertTrue(uncovered.isEmpty(),
                "templates with no render case in this test: " + uncovered);

        Set<String> stale = new TreeSet<>(covered);
        stale.removeAll(onDisk);
        assertTrue(stale.isEmpty(),
                "render cases naming templates that no longer exist: " + stale);
    }

    @Test
    void templatesWereActuallyFound() throws IOException {
        // A silent empty walk would make the coverage test above pass vacuously.
        List<String> found = templateFilesInSourceTree();
        assertTrue(found.size() >= 10, "expected the template tree, found: " + found);
    }

    /**
     * Template paths relative to the templates directory, e.g. "firms/list.ftl".
     *
     * Deliberately the source tree and not the classpath. Maven copies resources into
     * target/classes but never prunes ones that have been deleted, so a template removed
     * from the repo lingers there until a clean build -- and the coverage check above would
     * then fail claiming a file needs a render case when it no longer exists. Rendering
     * still goes through the classpath, via the production Freemarker configuration; it is
     * only this inventory that reads the source directory.
     *
     * Surefire runs with the working directory set to the module base directory.
     */
    private static List<String> templateFilesInSourceTree() throws IOException {
        Path root = Paths.get("src", "main", "resources", "templates");
        assertTrue(Files.isDirectory(root),
                "template source directory not found at " + root.toAbsolutePath()
                        + " (working directory is " + Paths.get("").toAbsolutePath() + ")");

        List<String> names = new ArrayList<>();
        try (Stream<Path> walk = Files.walk(root)) {
            walk.filter(Files::isRegularFile)
                    .filter(path -> path.getFileName().toString().endsWith(".ftl"))
                    .forEach(path -> names.add(
                            root.relativize(path).toString().replace('\\', '/')));
        }
        return names;
    }
}
