<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/static/pico.classless.min.css">
    <#if brandCss?has_content><style>${brandCss}</style></#if>
    <title>Setup – ${brandName}</title>
</head>
<body>
<main>
    <article style="max-width:26rem;margin:4rem auto">
        <#if brandLogoUrl?has_content>
        <p style="text-align:center;margin-bottom:0.5rem">
            <img src="${brandLogoUrl}" alt="${brandName}" style="max-height:3rem">
        </p>
        </#if>
        <h2>${brandName} – First-Run Setup</h2>
        <p>Create the initial administrator account.</p>
        <#if error??><p><mark>${error}</mark></p></#if>
        <form method="post" action="/setup">
            <label>Username
                <input type="text" name="username" value="${username!''}" required autofocus>
            </label>
            <label>Password
                <input type="password" name="password" required autocomplete="new-password">
            </label>
            <label>Confirm password
                <input type="password" name="confirm" required autocomplete="new-password">
            </label>
            <button type="submit">Create account</button>
        </form>
    </article>
</main>
</body>
</html>
