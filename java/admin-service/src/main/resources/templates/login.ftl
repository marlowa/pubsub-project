<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/static/pico.classless.min.css">
    <#if brandCss?has_content><style>${brandCss}</style></#if>
    <title>Sign in – ${brandName}</title>
</head>
<body>
<main>
    <article style="max-width:24rem;margin:4rem auto">
        <#if brandLogoUrl?has_content>
        <p style="text-align:center;margin-bottom:0.5rem">
            <img src="${brandLogoUrl}" alt="${brandName}" style="max-height:3rem">
        </p>
        </#if>
        <h2>${brandName}</h2>
        <#if error??><p><mark>${error}</mark></p></#if>
        <form method="post" action="/login">
            <label>Username
                <input type="text" name="username" required autofocus autocomplete="username">
            </label>
            <label>Password
                <input type="password" name="password" required autocomplete="current-password">
            </label>
            <button type="submit">Sign in</button>
        </form>
    </article>
</main>
</body>
</html>
