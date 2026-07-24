export interface PolicyDecision {
  decision: 'allow' | 'deny' | 'approval_required';
  reason?: string;
}

export interface CommandPolicy {
  authorize(input: {
    taskId: string;
    profile: string;
    command: string;
    cwd: string;
  }): Promise<PolicyDecision>;
}

/**
 * The only v1 profile. Keeping this interface at the execution boundary means
 * later profiles can be added without changing Pi or ShellExecutor.
 */
export class FullAccessPolicy implements CommandPolicy {
  async authorize(): Promise<PolicyDecision> {
    return { decision: 'allow' };
  }
}
