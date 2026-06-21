<#import "/templates/layout.ftl" as layout>
<#if firm??>
    <#assign pageTitle = "Edit Firm: " + firm.firmId()>
<#else>
    <#assign pageTitle = "New Firm">
</#if>
<@layout.page title=pageTitle>
<h1>${pageTitle}</h1>
<#if firm??>
<form method="post" action="/firms/${firm.firmId()}">
    <label for="firmId">Firm ID
        <input type="text" id="firmId" name="firmId"
               value="${firm.firmId()}" readonly>
    </label>
    <label for="name">Name
        <input type="text" id="name" name="name"
               value="${firm.name()}" required maxlength="255">
    </label>
    <label>
        <input type="checkbox" name="enabled" <#if firm.enabled()>checked</#if>>
        Enabled
    </label>
    <button type="submit">Update</button>
    <a href="/firms">← Firms</a>
</form>

<hr>
<h2>CompIDs</h2>
<#if compIds?has_content>
<table>
    <thead>
        <tr>
            <th>CompID</th>
            <th>Enabled</th>
            <th>Locked</th>
            <th>Force PW Change</th>
            <th>Actions</th>
        </tr>
    </thead>
    <tbody>
        <#list compIds as row>
        <tr>
            <td>${row.compId()}</td>
            <td>${row.enabled()?string("Yes","No")}</td>
            <td>${row.locked()?string("Yes","No")}</td>
            <td>${row.forcePasswordChange()?string("Yes","No")}</td>
            <td>
                <a href="/comp-ids/${row.compId()}">Edit</a> |
                <form method="post" action="/comp-ids/${row.compId()}/delete"
                      style="display:inline"
                      onsubmit="return confirm('Delete CompID ${row.compId()}?')">
                    <button type="submit">Delete</button>
                </form>
            </td>
        </tr>
        </#list>
    </tbody>
</table>
<#else>
<p>No CompIDs yet.</p>
</#if>

<h3>Add CompID</h3>
<form method="post" action="/firms/${firm.firmId()}/comp-ids">
    <div class="grid">
        <label for="compId">CompID
            <input type="text" id="compId" name="compId" required maxlength="64"
                   placeholder="e.g. ACME_TRADER1">
        </label>
        <label for="password">Initial Password
            <input type="password" id="password" name="password" required>
        </label>
    </div>
    <label>
        <input type="checkbox" name="forcePasswordChange" checked>
        Force password change on first login
    </label>
    <button type="submit">Add CompID</button>
</form>

<#else>
<form method="post" action="/firms">
    <label for="firmId">Firm ID
        <input type="text" id="firmId" name="firmId" required maxlength="32"
               placeholder="e.g. ACME">
    </label>
    <label for="name">Name
        <input type="text" id="name" name="name" required maxlength="255"
               placeholder="e.g. Acme Trading Ltd">
    </label>
    <button type="submit">Create</button>
    <a href="/firms">Cancel</a>
</form>
</#if>
</@layout.page>
