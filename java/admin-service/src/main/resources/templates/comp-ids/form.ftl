<#import "/templates/layout.ftl" as layout>
<#if row??>
    <#assign pageTitle = "Edit CompID: " + row.compId()>
<#else>
    <#assign pageTitle = "New CompID">
</#if>
<@layout.page title=pageTitle>
<h1>${pageTitle}</h1>
<#if row??>
<p>Firm: <a href="/comp-ids?firmId=${row.firmId()}">${row.firmId()}</a></p>
<form method="post" action="/comp-ids/${row.compId()}">
    <fieldset>
        <legend>Account Status</legend>
        <label>
            <input type="checkbox" name="enabled"
                   <#if row.enabled()>checked</#if>>
            Enabled
        </label>
        <label>
            <input type="checkbox" name="forcePasswordChange"
                   <#if row.forcePasswordChange()>checked</#if>>
            Force Password Change on Next Login
        </label>
        <label>
            <input type="checkbox" name="locked"
                   <#if row.locked()>checked</#if>>
            Locked
        </label>
        <label for="lockedReason">Locked Reason
            <input type="text" id="lockedReason" name="lockedReason"
                   value="${(row.lockedReason())!''}" maxlength="255">
        </label>
    </fieldset>
    <button type="submit">Update</button>
    <a href="/comp-ids/${row.compId()}/password" role="button">Set Password</a>
    <a href="/comp-ids/${row.compId()}/gateways">Gateways</a>
    <a href="/comp-ids?firmId=${row.firmId()}">Cancel</a>
</form>
<#else>
<form method="post" action="/firms/${firmId}/comp-ids">
    <label for="compId">CompID
        <input type="text" id="compId" name="compId" required maxlength="64"
               placeholder="e.g. ACME_TRADER1">
    </label>
    <label for="password">Initial Password
        <input type="password" id="password" name="password" required>
    </label>
    <label>
        <input type="checkbox" name="forcePasswordChange" checked>
        Force password change on first login
    </label>
    <button type="submit">Create</button>
    <a href="/comp-ids?firmId=${firmId}">Cancel</a>
</form>
</#if>
</@layout.page>
