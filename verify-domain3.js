const { google } = require('googleapis');

async function main() {
    const auth = new google.auth.GoogleAuth({
        keyFile: './sa.json',
        scopes: ['https://www.googleapis.com/auth/siteverification'],
    });

    const client = await auth.getClient();
    const siteVerification = google.siteVerification({ version: 'v1', auth: client });

    try {
        const res = await siteVerification.webResource.update({
            id: 'dns://browseaeon.com',
            requestBody: {
                site: {
                    identifier: "browseaeon.com",
                    type: "INET_DOMAIN"
                },
                owners: [
                    "aeon-cicd@aeon-browser-build.iam.gserviceaccount.com",
                    "chronolapse411@gmail.com"
                ]
            }
        });
        
        console.log("Updated owners!", res.data);
    } catch (err) {
        console.error("Failed to update owners", err.response?.data || err.message);
    }
}

main();
