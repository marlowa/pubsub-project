<#import "/templates/layout.ftl" as layout>
<@layout.page title="Set Password: ${compId}">
<h1>Set Password for ${compId}</h1>
<form method="post" action="/comp-ids/${compId}/password">
    <label for="password">New Password
        <input type="password" id="password" name="password" required
               autocomplete="new-password">
    </label>
    <button type="submit">Set Password</button>
    <a href="/comp-ids/${compId}">Cancel</a>
</form>
</@layout.page>
