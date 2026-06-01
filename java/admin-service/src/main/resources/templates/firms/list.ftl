<#import "/templates/layout.ftl" as layout>
<@layout.page title="Firms">
<hgroup>
    <h1>Firms</h1>
    <p><a href="/firms/new" role="button">+ New Firm</a></p>
</hgroup>
<table>
    <thead>
        <tr>
            <th>Firm ID</th>
            <th>Name</th>
            <th>Enabled</th>
            <th>Created</th>
            <th>Actions</th>
        </tr>
    </thead>
    <tbody>
        <#list firms as firm>
        <tr>
            <td>${firm.firmId()}</td>
            <td>${firm.name()}</td>
            <td>${firm.enabled()?string("Yes","No")}</td>
            <td>${firm.createdAt()!''}</td>
            <td>
                <a href="/firms/${firm.firmId()}">Edit</a> |
                <a href="/comp-ids?firmId=${firm.firmId()}">CompIDs</a> |
                <form method="post" action="/firms/${firm.firmId()}/delete"
                      style="display:inline"
                      onsubmit="return confirm('Delete firm ${firm.firmId()}?')">
                    <button type="submit">Delete</button>
                </form>
            </td>
        </tr>
        </#list>
        <#if firms?size == 0>
        <tr><td colspan="5">No firms defined.</td></tr>
        </#if>
    </tbody>
</table>
</@layout.page>
