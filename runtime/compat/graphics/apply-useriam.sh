set -e
B=/busybox
R=/hmos-root
$B gzip -dc /graphics/libdipper_userauth_4_0.so.gz > $R/compat/lib64/libdipper_userauth_4_0.so
echo "f23c1322446bab6901a8186f2ff543bb188b6c1365ceb8884810c4828a7cbc62  $R/compat/lib64/libdipper_userauth_4_0.so" | $B sha256sum -c -
$B chmod 0755 $R/compat/lib64/libdipper_userauth_4_0.so
$B gzip -dc /graphics/libuser_auth_interface_service_4.1.z.so.gz > $R/compat/lib64/libuser_auth_interface_service_4.1.z.so
echo "3ae1c6fe7f7b0f189fea8436771564e6f78caeb2bba779ac25341c58b42d6b1a  $R/compat/lib64/libuser_auth_interface_service_4.1.z.so" | $B sha256sum -c -
$B chmod 0755 $R/compat/lib64/libuser_auth_interface_service_4.1.z.so
$B gzip -dc /graphics/libdipper_user_auth_driver.so.gz > $R/compat/lib64/libdipper_user_auth_driver.so
echo "9eecdabc6815e5d5fe643c470610c2ede18bbdd18a1b2bd449cd08515d761331  $R/compat/lib64/libdipper_user_auth_driver.so" | $B sha256sum -c -
$B chmod 0755 $R/compat/lib64/libdipper_user_auth_driver.so
$B gzip -dc /graphics/useriam_empty_contract.gz > $R/compat/bin/useriam_empty_contract
echo "6af2b61c0e81c6f77e83e7610ec0654cc91e6fe281b98a9feae3d7ee4663532a  $R/compat/bin/useriam_empty_contract" | $B sha256sum -c -
$B chmod 0755 $R/compat/bin/useriam_empty_contract
echo DIPPER_SOFTWARE_USERAUTH_4_1_STAGED_VENDOR_UNTOUCHED
