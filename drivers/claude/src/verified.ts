// The private credential broker validates the account and email, and refreshes its grant under lock.
// Only its one-line bearer reaches this process. A provider error or diagnostic must never enter a transcript.
import { execFile } from 'node:child_process';

export interface VerifiedLogin {
    email: string;
    helper: string;
    claude: string;   // the vendor CLI used by the broker; independent of the SDK's executable
}

export type VerifiedAccess = { token: string } | 'sign-in' | 'unavailable';

export function verifiedAccess(login: VerifiedLogin, accountDir: string): Promise<VerifiedAccess> {
    return new Promise((resolve) => {
        execFile(login.helper, ['access', '--account-dir', accountDir, '--email', login.email, '--claude', login.claude], {
            encoding: 'utf8', cwd: '/', env: { PATH: '/usr/bin:/bin', LANG: 'C' },
            timeout: 60_000, maxBuffer: 64 * 1024,
        }, (error, stdout) => {
            if (error) {
                resolve(error.code === 11 || error.code === 12 ? 'sign-in' : 'unavailable');
            } else {
                const token = stdout.slice(0, -1);
                resolve(stdout.endsWith('\n') && token.length > 0 && !/[^!-~]/.test(token)
                    ? { token } : 'unavailable');
            }
        });
    });
}
