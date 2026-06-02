<#import "/templates/layout.ftl" as layout>
<@layout.page title="Change Password">
<article style="max-width:30rem">
    <h2>Change Password</h2>
    <#if error??><p><mark>${error}</mark></p></#if>
    <form method="post" action="/change-password">
        <label>Current password
            <input type="password" name="currentPassword" required autofocus
                   autocomplete="current-password">
        </label>
        <label>New password
            <input type="password" name="newPassword" required autocomplete="new-password">
        </label>
        <label>Confirm new password
            <input type="password" name="confirmPassword" required autocomplete="new-password">
        </label>
        <button type="submit">Change password</button>
    </form>
</article>
</@layout.page>
