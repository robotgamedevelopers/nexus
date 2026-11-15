//go:build windows

package main

import (
	"fmt"
	"os"

	"golang.org/x/sys/windows"
)

// ownerOnly gives the state directory a protected ACL (nothing inherited from its parents) with one entry: full
// control for the current user, inherited by everything inside it. A directory whose ACL this user may not change is
// refused.
func ownerOnly(dir string, _ os.FileInfo) error {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return fmt.Errorf("cannot read this process's user to make the state directory %s owner-only: %v", dir, err)
	}
	acl, err := windows.ACLFromEntries([]windows.EXPLICIT_ACCESS{{
		AccessPermissions: windows.GENERIC_ALL,
		AccessMode:        windows.SET_ACCESS,
		Inheritance:       windows.SUB_CONTAINERS_AND_OBJECTS_INHERIT,
		Trustee: windows.TRUSTEE{
			TrusteeForm:  windows.TRUSTEE_IS_SID,
			TrusteeType:  windows.TRUSTEE_IS_USER,
			TrusteeValue: windows.TrusteeValueFromSID(user.User.Sid),
		},
	}}, nil)
	if err != nil {
		return fmt.Errorf("cannot build the owner-only ACL for the state directory %s: %v", dir, err)
	}
	err = windows.SetNamedSecurityInfo(dir, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION, nil, nil, acl, nil)
	if err != nil {
		return fmt.Errorf("cannot make the state directory %s owner-only (an ACL for this user alone): %v; give a "+
			"directory of this user's own", dir, err)
	}
	return nil
}

// ownerOnlyFile has nothing to do on Windows: the log file inherits the directory's owner-only ACL.
func ownerOnlyFile(*os.File) error {
	return nil
}
