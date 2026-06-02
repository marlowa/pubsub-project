<#import "/templates/layout.ftl" as layout>
<@layout.page title="Admin Users">
<hgroup>
    <h1>Admin Users</h1>
    <p><a href="/admin/users/new" role="button">+ New User</a></p>
</hgroup>
<#if error??><p><mark>${error}</mark></p></#if>
<table>
    <thead>
        <tr>
            <th>Username</th>
            <th>Role</th>
            <th>Force Password Change</th>
            <th>Actions</th>
        </tr>
    </thead>
    <tbody>
        <#list users as user>
        <tr>
            <td>${user.username()}</td>
            <td>${user.role()}</td>
            <td>${user.forcePasswordChange()?string("Yes","No")}</td>
            <td>
                <a href="/admin/users/${user.username()}/edit">Edit role</a> |
                <a href="/admin/users/${user.username()}/reset-password">Reset password</a> |
                <form method="post" action="/admin/users/${user.username()}/delete"
                      style="display:inline"
                      onsubmit="return confirm('Delete user ${user.username()}?')">
                    <button type="submit">Delete</button>
                </form>
            </td>
        </tr>
        </#list>
        <#if users?size == 0>
        <tr><td colspan="4">No users defined.</td></tr>
        </#if>
    </tbody>
</table>
</@layout.page>
