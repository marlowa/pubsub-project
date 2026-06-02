<#macro page title>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/static/pico.classless.min.css">
    <#if brandCss?has_content><style>${brandCss}</style></#if>
    <title>${title} – ${brandName}</title>
</head>
<body>
<header>
    <nav>
        <ul>
            <li>
                <#if brandLogoUrl?has_content>
                <img src="${brandLogoUrl}" alt="${brandName}"
                     style="height:1.5rem;vertical-align:middle;margin-right:0.4rem">
                </#if>
                <strong>${brandName}</strong>
            </li>
        </ul>
        <ul>
            <li><a href="/firms">Firms</a></li>
            <li><a href="/comp-ids">CompIDs</a></li>
            <#if isAdmin!false><li><a href="/admin/users">Users</a></li></#if>
        </ul>
        <ul>
            <#if currentUser??>
            <li><small>${currentUser}</small></li>
            <li>
                <form method="post" action="/logout" style="display:inline;margin:0">
                    <button type="submit" style="padding:0.25rem 0.75rem">Logout</button>
                </form>
            </li>
            </#if>
        </ul>
    </nav>
</header>
<main>
    <#nested>
</main>
</body>
</html>
</#macro>
