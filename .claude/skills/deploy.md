---
name: deploy
description: Deploy the project to production
---

# Deployment

## Pre-Deployment Checklist
- [ ] All tests pass: `/test`
- [ ] Code is linted: `/lint`
- [ ] No pending changes: `git status`
- [ ] Latest main branch: `git pull origin main`

## Build for Production
```bash
npm run build
```

## Verify Build
```bash
npm run build:verify
```

## Deploy Steps
1. Build production bundle
   ```bash
   npm run build
   ```

2. Deploy to [TODO] environment
   ```bash
   npm run deploy
   ```

3. Verify deployment
   ```bash
   [TODO] Add verification command
   ```

## Rollback (if needed)
```bash
# Rollback to previous version
[TODO] Add rollback command
```

## Environment-Specific Deployment
### Staging
```bash
npm run deploy:staging
```

### Production
```bash
npm run deploy:production
```

## Deployment Checklist
- [ ] Build successful
- [ ] No errors in logs
- [ ] Deployment confirmed
- [ ] Health checks passing
- [ ] Alerts configured

## Post-Deployment
- Monitor application logs
- Check error tracking (e.g., Sentry)
- Verify critical features

## Emergency Contacts
- [TODO] Add on-call contact
- [TODO] Add escalation path
