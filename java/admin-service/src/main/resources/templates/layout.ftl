<#macro page title>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet"
          href="https://cdn.jsdelivr.net/npm/@picocss/pico@2/css/pico.classless.min.css">
    <title>${title} – PubSub Admin</title>
</head>
<body>
<header>
    <nav>
        <ul>
            <li><strong>PubSub Admin</strong></li>
        </ul>
        <ul>
            <li><a href="/firms">Firms</a></li>
            <li><a href="/comp-ids">CompIDs</a></li>
        </ul>
    </nav>
</header>
<main>
    <#nested>
</main>
</body>
</html>
</#macro>
