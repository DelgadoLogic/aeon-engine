const { google } = require('googleapis');

async function main() {
    const auth = new google.auth.GoogleAuth({
        keyFile: './sa.json',
        scopes: ['https://www.googleapis.com/auth/siteverification'],
    });

    const client = await auth.getClient();
    const siteVerification = google.siteVerification({ version: 'v1', auth: client });

    const domains = ["aeonbrowse.com", "aeonbrowser.dev"];

    for (const domain of domains) {
        console.log(`Getting token for ${domain}...`);
        try {
            const tokenRes = await siteVerification.webResource.getToken({
                requestBody: {
                    verificationMethod: "DNS_TXT",
                    site: {
                        identifier: domain,
                        type: "INET_DOMAIN"
                    }
                }
            });
            
            const tokenStr = tokenRes.data.token;
            console.log(`[TOKEN:${domain}]`, tokenStr);

            // push to cloudflare via Powershell internally here using node fetch
            // But we'll just log it so PS can see it, and I'll run the PS next.
        } catch (err) {
            console.error(`Failed to get token for ${domain}`, err);
        }
    }
}

main();
