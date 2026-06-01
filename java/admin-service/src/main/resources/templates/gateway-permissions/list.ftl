<#import "/templates/layout.ftl" as layout>
<@layout.page title="Gateway Permissions: ${compId}">
<hgroup>
    <h1>Gateway Permissions for ${compId}</h1>
    <p><a href="/comp-ids/${compId}">← Back to CompID</a></p>
</hgroup>
<table>
    <thead>
        <tr>
            <th>Gateway Type</th>
            <th>Enabled</th>
            <th>Created</th>
            <th>Actions</th>
        </tr>
    </thead>
    <tbody>
        <#list permissions as perm>
        <tr>
            <td>${perm.gatewayType()}</td>
            <td>${perm.enabled()?string("Yes","No")}</td>
            <td>${perm.createdAt()!''}</td>
            <td>
                <form method="post"
                      action="/comp-ids/${compId}/gateways/${perm.gatewayType()}/delete"
                      style="display:inline"
                      onsubmit="return confirm('Remove ${perm.gatewayType()}?')">
                    <button type="submit">Remove</button>
                </form>
            </td>
        </tr>
        </#list>
        <#if permissions?size == 0>
        <tr><td colspan="4">No gateway permissions defined.</td></tr>
        </#if>
    </tbody>
</table>
<h2>Add Gateway Permission</h2>
<form method="post" action="/comp-ids/${compId}/gateways">
    <label for="gatewayType">Gateway Type
        <input type="text" id="gatewayType" name="gatewayType" required maxlength="64"
               placeholder="e.g. order, drop_copy, risk">
    </label>
    <button type="submit">Add</button>
</form>
</@layout.page>
