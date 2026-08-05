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
    <fieldset>
        <legend>Cancel on Disconnect</legend>
        <small>
            What the gateway does with this member's resting orders when its connection
            goes away. Leaving the grace period blank uses the gateway's own configured
            default, which is not the same as entering 0 &mdash; 0 cancels immediately.
        </small>
        <label>
            <input type="checkbox" name="cancelOnDisconnectEnabled"
                   <#if row.cancelOnDisconnectEnabled()>checked</#if>>
            Cancel resting orders when the session disconnects
        </label>
        <label for="cancelOnDisconnectGracePeriodSeconds">Grace Period (seconds)
            <input type="number" id="cancelOnDisconnectGracePeriodSeconds"
                   name="cancelOnDisconnectGracePeriodSeconds" min="0" step="1"
                   placeholder="gateway default"
                   value="${(row.cancelOnDisconnectGracePeriodSeconds()?c)!''}">
        </label>
        <small>
            Good-Till-Cancel and Good-Till-Date orders are never cancelled on disconnect,
            whatever is set here: they were placed to outlive the session. A clean FIX
            Logout cancels immediately, since the member has said what it wants.
        </small>
    </fieldset>
    <fieldset>
        <legend>Gateway Session Provisioning</legend>
        <small>
            The gateway instances this member's session may log on to. A gateway refuses a
            logon from a member pinned elsewhere. Leave both blank to leave the member
            unpinned, free to use any instance; set a primary alone to pin it to exactly
            one. Instances are numbered from 1 &mdash; instance 1 is the <em>_a</em>
            process, 2 is <em>_b</em> &mdash; and name an instance of whichever order-entry
            protocol the member speaks, not a protocol of its own.
        </small>
        <label for="primaryGatewayInstance">Primary Gateway Instance
            <input type="number" id="primaryGatewayInstance"
                   name="primaryGatewayInstance" min="1" step="1"
                   placeholder="not pinned"
                   value="${(row.primaryGatewayInstance()?c)!''}">
        </label>
        <label for="backupGatewayInstance">Backup Gateway Instance
            <input type="number" id="backupGatewayInstance"
                   name="backupGatewayInstance" min="1" step="1"
                   placeholder="none"
                   value="${(row.backupGatewayInstance()?c)!''}">
        </label>
        <small>
            A backup must differ from the primary, and needs a primary to be the backup of.
        </small>
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
