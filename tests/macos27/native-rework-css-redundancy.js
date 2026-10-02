async (page) => {
    await page.goto('http://127.0.0.1:18783/?state=populated#/');
    await page.waitForFunction(()=>[...document.styleSheets].some(s=>s.href?.includes('/css/style.css')));
    return page.evaluate(()=>{
        const sheet=[...document.styleSheets].find(s=>s.href?.includes('/css/style.css'));
        const rows=[];
        function walk(rules,context=[]) {
            for(const rule of rules) {
                if(rule.type===CSSRule.STYLE_RULE) rows.push({context:context.join(' / '),selector:rule.selectorText,
                    declarations:[...rule.style].map(property=>({property,value:rule.style.getPropertyValue(property),priority:rule.style.getPropertyPriority(property)})),css:rule.cssText});
                else if(rule.type===CSSRule.MEDIA_RULE||rule.type===CSSRule.SUPPORTS_RULE) walk(rule.cssRules,[...context,rule.conditionText]);
            }
        }
        walk(sheet.cssRules);
        const candidates=[];
        for(let i=0;i<rows.length;i++) {
            const row=rows[i],covered=row.declarations.filter(d=>rows.slice(i+1).some(next=>next.context===row.context&&next.selector===row.selector&&next.declarations.some(n=>n.property===d.property&&(!d.priority||n.priority===d.priority))));
            if(covered.length) candidates.push({...row,covered,fullyOverridden:covered.length===row.declarations.length});
        }
        return {scope:'Read-only duplicate declaration candidates. Browser CSSOM is not source editing or visual acceptance.',ruleCount:rows.length,candidates};
    });
}
