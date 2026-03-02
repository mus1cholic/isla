module.exports = async ({ github, context }) => {
    const fs = require('fs');
    const path = require('path');
    const marker = '<!-- clang-tidy-report -->';
    const prNumber = Number(process.env.PR_NUMBER);
    function findFileRecursive(rootDir, targetFile) {
        if (!fs.existsSync(rootDir)) return null;
        const stack = [rootDir];
        while (stack.length > 0) {
            const dir = stack.pop();
            const entries = fs.readdirSync(dir, { withFileTypes: true });
            for (const entry of entries) {
                const full = path.join(dir, entry.name);
                if (entry.isDirectory()) {
                    stack.push(full);
                } else if (entry.isFile() && entry.name === targetFile) {
                    return full;
                }
            }
        }
        return null;
    }

    let formatText = 'No format violations.';
    let formatCount = 0;
    const formatReportPath = findFileRecursive('format-report', 'format-check-warnings.md');
    const formatCountPath = findFileRecursive('format-report', 'format-count.txt');
    if (formatReportPath) {
        formatText = fs.readFileSync(formatReportPath, 'utf8').trim();
    }
    if (formatCountPath) {
        formatCount = Number(fs.readFileSync(formatCountPath, 'utf8').trim() || '0');
    }

    let lintText = [
        '### clang-tidy warnings/errors (0)',
        '',
        'No clang-tidy warnings or errors.',
        '',
        '### compiler warnings/errors (0)',
        '',
        'No compiler warnings or errors.'
    ].join('\n');
    const lintReportPath = findFileRecursive('lint-report', 'clang-tidy-warnings.md');
    if (lintReportPath) {
        lintText = fs.readFileSync(lintReportPath, 'utf8').trim();
    }

    const { data: comments } = await github.rest.issues.listComments({
        owner: context.repo.owner,
        repo: context.repo.repo,
        issue_number: prNumber,
        per_page: 100
    });

    const existingComments = comments.filter(c =>
        c.user?.type === 'Bot' && c.body?.includes(marker)
    );

    const formatSection = [
        '## format check report',
        `Violations: **${formatCount}**`,
        '',
        formatText
    ].join('\n');

    const body = [
        marker,
        '## clang-tidy report',
        lintText,
        '',
        formatSection,
        '',
        '_This comment is updated automatically by CI._'
    ].join('\n');

    for (const c of existingComments) {
        try {
            await github.rest.issues.deleteComment({
                owner: context.repo.owner,
                repo: context.repo.repo,
                comment_id: c.id
            });
        } catch (e) {
            console.log('Failed to delete comment', c.id, e);
        }
    }
    await github.rest.issues.createComment({
        owner: context.repo.owner,
        repo: context.repo.repo,
        issue_number: prNumber,
        body
    });
};
