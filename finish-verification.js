const { google } = require('googleapis');
const fs = require('fs');

async function main() {
    const auth = new google.auth.GoogleAuth({
        keyFile: './sa.json',
        scopes: ['https://www.googleapis.com/auth/siteverification'],
    });

    const client = await auth.getClient();
    const siteVerification = google.siteVerification({ version: 'v1', auth: client });

    const domain = "browseaeon.com";

    console.log(`Verifying ${domain}...`);
    try {
        const insertRes = await siteVerification.webResource.insert({
            verificationMethod: "DNS_TXT",
            requestBody: {
                site: {
                    identifier: domain,
                    type: "INET_DOMAIN"
                },
                owners: [
                    "aeon-cicd@aeon-browser-build.iam.gserviceaccount.com",
                    "chronolapse411@gmail.com"
                ]
            }
        });
        
        console.log("VERIFIED SUCCESSFULLY:", insertRes.data.id);
    } catch (err) {
        console.error("Failed to verify", err);
    }
}

main();
