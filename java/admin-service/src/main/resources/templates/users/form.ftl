<#import "/templates/layout.ftl" as layout>
<@layout.page title="${user??'Edit User':'New User'}">
<article style="max-width:30rem">
    <h2><#if user??>Edit User: ${user.username()}<#else>New User</#if></h2>
    <#if error??><p><mark>${error}</mark></p></#if>
    <#if user??>
    <form method="post" action="/admin/users/${user.username()}">
    <#else>
    <form method="post" action="/admin/users">
    </#if>
        <#if !user??>
        <label>Username
            <input type="text" name="username" required autofocus autocomplete="off">
        </label>
        <label>Password
            <input type="password" name="password" required autocomplete="new-password">
        </label>
        </#if>
        <label>Role
            <select name="role">
                <option value="VIEWER"<#if user?? && user.role().name() == "VIEWER"> selected</#if>>
                    Viewer (read-only)
                </option>
                <option value="ADMIN"<#if user?? && user.role().name() == "ADMIN"> selected</#if>>
                    Admin (full access)
                </option>
            </select>
        </label>
        <button type="submit"><#if user??>Save<#else>Create User</#if></button>
        <a href="/admin/users">Cancel</a>
    </form>
</article>
</@layout.page>
