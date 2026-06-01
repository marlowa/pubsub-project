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
    <a href="/firms">Cancel</a>
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
