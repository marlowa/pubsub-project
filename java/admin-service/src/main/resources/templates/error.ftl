<#import "/templates/layout.ftl" as layout>
<@layout.page title="Error">
<article>
    <header><h2>Error</h2></header>
    <p>${message!''}</p>
    <footer><a href="javascript:history.back()">Go back</a></footer>
</article>
</@layout.page>
