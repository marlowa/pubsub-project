<#import "/templates/layout.ftl" as layout>
<#if firmId??>
    <#assign pageTitle = "CompIDs for Firm: " + firmId>
<#else>
    <#assign pageTitle = "All CompIDs">
</#if>
<@layout.page title=pageTitle>
<hgroup>
    <h1>${pageTitle}</h1>
    <#if firmId??>
    <p><a href="/firms/${firmId}/comp-ids/new" role="button">+ New CompID</a></p>
    </#if>
</hgroup>
<table>
    <thead>
        <tr>
            <th>CompID</th>
            <th>Firm</th>
            <th>Enabled</th>
            <th>Locked</th>
            <th>Force PW Change</th>
            <th>Failed Logins</th>
            <th>Actions</th>
        </tr>
    </thead>
    <tbody>
        <#list compIds as row>
        <tr>
            <td>${row.compId()}</td>
            <td>${row.firmId()}</td>
            <td>${row.enabled()?string("Yes","No")}</td>
            <td>${row.locked()?string("Yes","No")}</td>
            <td>${row.forcePasswordChange()?string("Yes","No")}</td>
            <td>${row.consecutiveFailedLogins()}</td>
            <td>
                <a href="/comp-ids/${row.compId()}">Edit</a> |
                <a href="/comp-ids/${row.compId()}/password">Password</a> |
                <a href="/comp-ids/${row.compId()}/gateways">Gateways</a> |
                <form method="post" action="/comp-ids/${row.compId()}/delete"
                      style="display:inline"
                      onsubmit="return confirm('Delete CompID ${row.compId()}?')">
                    <button type="submit">Delete</button>
                </form>
            </td>
        </tr>
        </#list>
        <#if compIds?size == 0>
        <tr><td colspan="7">No CompIDs found.</td></tr>
        </#if>
    </tbody>
</table>
<a href="/firms">← Firms</a>
</@layout.page>
