// Lint: typescript-eslint's strict and stylistic rules with type information, warnings as errors (npm run lint).
import js from '@eslint/js';
import tseslint from 'typescript-eslint';

export default tseslint.config(
    { ignores: ['dist/', 'node_modules/', 'eslint.config.mjs'] },
    js.configs.recommended,
    ...tseslint.configs.strictTypeChecked,
    ...tseslint.configs.stylisticTypeChecked,
    {
        languageOptions: { parserOptions: { projectService: true, tsconfigRootDir: import.meta.dirname } },
        rules: {
            '@typescript-eslint/no-explicit-any': 'error',
            '@typescript-eslint/restrict-template-expressions': ['error', { allowNumber: true }],
        },
    },
);
