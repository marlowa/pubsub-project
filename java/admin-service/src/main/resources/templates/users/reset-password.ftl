<#import "/templates/layout.ftl" as layout>
<@layout.page title="Reset Password">
<article style="max-width:30rem">
    <h2>Reset Password: ${username}</h2>
    <#if error??><p><mark>${error}</mark></p></#if>
    <form method="post" action="/admin/users/${username}/reset-password">
        <label>New password
            <input type="password" name="newPassword" required autofocus
                   autocomplete="new-password">
        </label>
        <button type="submit">Reset password</button>
        <a href="/admin/users">Cancel</a>
    </form>
</article>
</@layout.page>
