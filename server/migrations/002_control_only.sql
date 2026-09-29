INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT user_id,device_id,'device.grant.revoke_readonly',TRUE,'control_only_migration' FROM device_grants WHERE NOT write_control AND (desktop OR vnc OR ssh);
UPDATE device_grants SET desktop=FALSE,vnc=FALSE,ssh=FALSE,system_restart=FALSE,manage_credentials=FALSE,require_approval=FALSE,revision=NEXT VALUE FOR grant_revision_sequence WHERE NOT write_control AND (desktop OR vnc OR ssh);
UPDATE connection_approvals SET state='cancelled' WHERE NOT write_control AND state IN ('pending','approved');
INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',TRUE,'control_only_migration' FROM remote_sessions WHERE NOT write_control AND state<>'closed';
UPDATE remote_sessions SET state='closed',ended_at=now(6),end_reason='control_only_migration' WHERE NOT write_control AND state<>'closed';
ALTER TABLE device_grants ADD CONSTRAINT grants_control_only CHECK (write_control OR NOT (desktop OR vnc OR ssh));
ALTER TABLE remote_sessions ADD CONSTRAINT sessions_control_only CHECK (write_control OR state='closed');
ALTER TABLE connection_approvals ADD CONSTRAINT approvals_control_only CHECK (write_control OR state NOT IN ('pending','approved'));
UPDATE device_grants g JOIN devices d ON d.id=g.device_id SET g.system_restart=FALSE,g.manage_credentials=FALSE,g.revision=NEXT VALUE FOR grant_revision_sequence WHERE g.user_id<>d.owner_id AND (g.system_restart OR g.manage_credentials);
