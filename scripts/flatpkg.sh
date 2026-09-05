# put the store's exports farm on the application search path.
FLATPKG_EXPORTS_DIR="/var/db/flatpkg/exports/share"
case ":${XDG_DATA_DIRS:-}:" in
	*:"${FLATPKG_EXPORTS_DIR}":*) ;;
	*) XDG_DATA_DIRS="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${FLATPKG_EXPORTS_DIR}" ;;
esac
export XDG_DATA_DIRS
