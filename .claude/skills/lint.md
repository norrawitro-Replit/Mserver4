---
name: lint
description: Check and fix code quality issues
---

# Lint & Format Code

## Check Code Quality
1. Run linter
   ```bash
   npm run lint
   ```

2. Check TypeScript (if applicable)
   ```bash
   npm run typecheck
   ```

3. Check formatting
   ```bash
   npm run format:check
   ```

## Fix Issues Automatically
1. Fix lint issues
   ```bash
   npm run lint:fix
   ```

2. Fix formatting
   ```bash
   npm run format
   ```

## Full Quality Check
```bash
npm run lint && npm run typecheck && npm run format:check
```

## Expected Results
- [TODO] No lint errors
- [TODO] No TypeScript errors
- [TODO] Code is properly formatted

## Configuration Files
- [TODO] `.eslintrc.json` - ESLint config
- [TODO] `.prettierrc` - Prettier config
- [TODO] `tsconfig.json` - TypeScript config (if applicable)

## Notes
- Run `/lint` before committing
- Fix formatting automatically with `/lint`
