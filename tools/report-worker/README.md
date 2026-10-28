# Problem reports server

A Cloudflare Worker that receives the ZIPs sent by the report window (`scripts/enhanced/report.ps1`: opened
by the launcher after a crash, by the Enhanced menu's "Send feedback" button and `Report a problem.bat`) and keeps them in a private R2 bucket. Free plan limits are plenty.

## Setting it up (once, from this folder)

```
npx wrangler login
npx wrangler r2 bucket create jak-enhanced-reports
npx wrangler secret put ADMIN_KEY
npx wrangler deploy
```

`deploy` prints the address (`https://jak-enhanced-reports.<your-subdomain>.workers.dev`): put it
with `/report` at the end in `$ReportUrl` at the top of `scripts/enhanced/report.ps1`.

## Reading the reports

```
curl -H "Authorization: Bearer <ADMIN_KEY>" https://<address>/reports
curl -H "Authorization: Bearer <ADMIN_KEY>" https://<address>/reports/<id>/note
curl -H "Authorization: Bearer <ADMIN_KEY>" -o report.zip https://<address>/reports/<id>
```

At most 5 reports an hour from one address, 8 MB each. Trying it locally: `npx wrangler dev`, with
`ADMIN_KEY=...` in `.dev.vars`.
