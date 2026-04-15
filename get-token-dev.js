const { google } = require('googleapis');

async function main() {
    const auth = new google.auth.GoogleAuth({
        keyFile: './sa.json',
        scopes: ['https://www.googleapis.com/auth/siteverification'],
    });

    const client = await auth.getClient();
    const siteVerification = google.siteVerification({ version: 'v1', auth: client });

    const domain = "aeonbrowser.dev";

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
        
        console.log(`[TOKEN]`, tokenRes.data.token);
    } catch (err) {
        console.error(`Failed to get token`, err.response?.data || err.message);
    }
}

main();
