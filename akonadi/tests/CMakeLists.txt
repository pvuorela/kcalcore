if(${EXECUTABLE_OUTPUT_PATH})
    set( PREVIOUS_EXEC_OUTPUT_PATH ${EXECUTABLE_OUTPUT_PATH} )
else()
    set( PREVIOUS_EXEC_OUTPUT_PATH . )
endif()
set( EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR} )
set( TEST_RESULT_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/testresults )
file(MAKE_DIRECTORY ${TEST_RESULT_OUTPUT_PATH})

option(KDEPIMLIBS_RUN_ISOLATED_TESTS "Run the isolated tests." FALSE)
option(KDEPIMLIBS_TESTS_XML "Use XML files for the test results, instead of plain text." FALSE)
option(KDEPIMLIBS_TESTS_SQLITE "Run all isolated tests with sqlite3 as backend in addition to the default backends" FALSE)

set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${KDE4_ENABLE_EXCEPTIONS}" )

include_directories(
    ${CMAKE_SOURCE_DIR}/akonadi
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../
    ${Boost_INCLUDE_DIR}
    ${AKONADI_INCLUDE_DIR}
    ${AKONADI_INCLUDE_DIR}/akonadi/private
)

# add testrunner (application for managing a self-contained test
# environment)

add_subdirectory(etm_test_app)
add_subdirectory(testrunner)

# add benchmarker
add_subdirectory(benchmarker)

# convenience macro to add akonadi demo application
macro(add_akonadi_demo _source)
  set(_test ${_source})
  get_filename_component(_name ${_source} NAME_WE)
  kde4_add_executable(${_name} TEST ${_test})
  target_link_libraries(${_name} akonadi-kde akonadi-kmime ${QT_QTCORE_LIBRARY} ${QT_QTGUI_LIBRARY} ${KDE4_KDECORE_LIBS} ${KDE4_KDEUI_LIBS})
endmacro()

# convenience macro to add akonadi qtestlib unit-tests
macro(add_akonadi_test _source)
  set(_test ${_source})
  get_filename_component(_name ${_source} NAME_WE)
  kde4_add_unit_test(${_name} TESTNAME akonadi-${_name} ${_test})
  target_link_libraries(${_name} akonadi-kde akonadi-kmime akonaditestfake ${QT_QTTEST_LIBRARY} ${QT_QTCORE_LIBRARY} ${QT_QTNETWORK_LIBRARY} ${QT_QTGUI_LIBRARY} ${KDE4_KDECORE_LIBS} ${AKONADI_COMMON_LIBRARIES})
endmacro()

# convenience macro to add akonadi testrunner unit-tests
macro(add_akonadi_isolated_test _source)
  set(_test ${_source})
  get_filename_component(_name ${_source} NAME_WE)
  kde4_add_executable(${_name} TEST ${_test})
  target_link_libraries(${_name} akonadi-kde akonadi-kmime ${QT_QTTEST_LIBRARY} ${QT_QTCORE_LIBRARY} ${QT_QTGUI_LIBRARY} ${KDE4_KDECORE_LIBS} ${AKONADI_COMMON_LIBRARIES})

  # based on kde4_add_unit_test
  if (WIN32)
    get_target_property( _loc ${_name} LOCATION )
    set(_executable ${_loc}.bat)
    set(_testrunner ${PREVIOUS_EXEC_OUTPUT_PATH}/akonaditest.exe.bat)
  else()
    set(_executable ${EXECUTABLE_OUTPUT_PATH}/${_name})
    set(_testrunner ${PREVIOUS_EXEC_OUTPUT_PATH}/akonaditest)
  endif()
  if (UNIX)
    if (APPLE)
      set(_executable ${_executable}.app/Contents/MacOS/${_name})
    else()
      set(_executable ${_executable}.shell)
    endif()
    set(_testrunner ${_testrunner}.shell)
  endif()

  if ( KDEPIMLIBS_TESTS_XML )
    set( MYSQL_EXTRA_OPTIONS_DB -xml -o ${TEST_RESULT_OUTPUT_PATH}/mysql-db-${_name}.xml )
    set( MYSQL_EXTRA_OPTIONS_FS -xml -o ${TEST_RESULT_OUTPUT_PATH}/mysql-fs-${_name}.xml )
    set( POSTGRESL_EXTRA_OPTIONS_DB -xml -o ${TEST_RESULT_OUTPUT_PATH}/postgresql-db-${_name}.xml )
    set( POSTGRESL_EXTRA_OPTIONS_FS -xml -o ${TEST_RESULT_OUTPUT_PATH}/postgresql-fs-${_name}.xml )
    set( SQLITE_EXTRA_OPTIONS -xml -o ${TEST_RESULT_OUTPUT_PATH}/sqlite-${_name}.xml )
  endif()

  if ( KDEPIMLIBS_RUN_ISOLATED_TESTS )
    find_program( MYSQLD_EXECUTABLE mysqld /usr/sbin /usr/local/sbin /usr/libexec /usr/local/libexec /opt/mysql/libexec /usr/mysql/bin )
    if ( MYSQLD_EXECUTABLE )
      add_test( akonadi-mysql-db-${_name} ${_testrunner} -c ${CMAKE_CURRENT_SOURCE_DIR}/unittestenv/config-mysql-db.xml ${_executable}
        ${MYSQL_EXTRA_OPTIONS_DB} )
      add_test( akonadi-mysql-fs-${_name} ${_testrunner} -c ${CMAKE_CURRENT_SOURCE_DIR}/unittestenv/config-mysql-fs.xml ${_executable}
        ${MYSQL_EXTRA_OPTIONS_FS} )
    endif()

    find_program( POSTGRES_EXECUTABLE postgres )
    if ( POSTGRES_EXECUTABLE )
      add_test( akonadi-postgresql-db-${_name} ${_testrunner} -c ${CMAKE_CURRENT_SOURCE_DIR}/unittestenv/config-postgresql-db.xml ${_executable}
        ${POSTGRESL_EXTRA_OPTIONS_DB} )
      add_test( akonadi-postgresql-fs-${_name} ${_testrunner} -c ${CMAKE_CURRENT_SOURCE_DIR}/unittestenv/config-postgresql-fs.xml ${_executable}
        ${POSTGRESL_EXTRA_OPTIONS_FS} )
    endif()

    if ( KDEPIMLIBS_TESTS_SQLITE )
      add_test( akonadi-sqlite-${_name} ${_testrunner} -c ${CMAKE_CURRENT_SOURCE_DIR}/unittestenv/config-sqlite-db.xml ${_executable}
        ${SQLITE_EXTRA_OPTIONS} )
    endif()
  endif()
endmacro()


# akonadi test fake library
set(akonaditestfake_xml ${AKONADI_DBUS_INTERFACES_DIR}/org.freedesktop.Akonadi.NotificationManager.xml)
set_source_files_properties(${akonaditestfake_xml} PROPERTIES INCLUDE "notificationmessage_p.h")
qt4_add_dbus_interface( akonaditestfake_srcs ${akonaditestfake_xml} notificationmanagerinterface )

set(akonaditestfake_xml ${AKONADI_DBUS_INTERFACES_DIR}/org.freedesktop.Akonadi.NotificationSource.xml)
set_source_files_properties(${akonaditestfake_xml} PROPERTIES INCLUDE "notificationmessage_p.h")
qt4_add_dbus_interface( akonaditestfake_srcs ${akonaditestfake_xml} notificationsourceinterface )

kde4_add_library(akonaditestfake STATIC
  ${akonaditestfake_srcs}
  fakeakonadiservercommand.cpp
  fakesession.cpp
  fakemonitor.cpp
  fakeserverdata.cpp
  modelspy.cpp
  fakeentitycache.cpp
  inspectablemonitor.cpp
  inspectablechangerecorder.cpp
)

# demo applications
add_akonadi_demo(itemdumper.cpp)
add_akonadi_demo(subscriber.cpp)
add_akonadi_demo(headfetcher.cpp)
add_akonadi_demo(agentinstancewidgettest.cpp)
add_akonadi_demo(agenttypewidgettest.cpp)
add_akonadi_demo(pluginloadertest.cpp)
add_akonadi_demo(selftester.cpp)
add_akonadi_demo(collectiondialog.cpp)

kde4_add_executable( akonadi-firstrun TEST ../firstrun.cpp firstrunner.cpp )
target_link_libraries( akonadi-firstrun akonadi-kde ${KDE4_KDEUI_LIBS} )

# qtestlib unit tests
add_akonadi_test(imapparsertest.cpp)
add_akonadi_test(imapsettest.cpp)
add_akonadi_test(itemhydratest.cpp)
add_akonadi_test(itemtest.cpp)
add_akonadi_test(itemserializertest.cpp)
add_akonadi_test(mimetypecheckertest.cpp)
add_akonadi_test(protocolhelpertest.cpp)
add_akonadi_test(entitytreemodeltest.cpp)
add_akonadi_test(monitornotificationtest.cpp)
add_akonadi_test(collectionutilstest.cpp)
add_akonadi_test(entitydisplayattributetest.cpp)
add_akonadi_test(proxymodelstest.cpp)
add_akonadi_test(actionstatemanagertest.cpp)
add_akonadi_test(sharedvaluepooltest.cpp)
add_akonadi_test(jobtest.cpp)

# qtestlib tests that need non-exported stuff from akonadi-kde
kde4_add_unit_test(resourceschedulertest TESTNAME akonadi-resourceschedulertest resourceschedulertest.cpp ../resourcescheduler.cpp)
target_link_libraries(resourceschedulertest akonadi-kde ${QT_QTTEST_LIBRARY} ${QT_QTCORE_LIBRARY} ${QT_QTGUI_LIBRARY} ${KDE4_KDECORE_LIBS} ${AKONADI_COMMON_LIBRARIES})


# testrunner tests
add_akonadi_isolated_test(testenvironmenttest.cpp)
add_akonadi_isolated_test(autoincrementtest.cpp)
add_akonadi_isolated_test(attributefactorytest.cpp)
add_akonadi_isolated_test(collectionjobtest.cpp)
add_akonadi_isolated_test(collectionpathresolvertest.cpp)
add_akonadi_isolated_test(collectionattributetest.cpp)
add_akonadi_isolated_test(itemfetchtest.cpp)
add_akonadi_isolated_test(itemappendtest.cpp)
add_akonadi_isolated_test(itemstoretest.cpp)
add_akonadi_isolated_test(itemdeletetest.cpp)
add_akonadi_isolated_test(entitycachetest.cpp)
add_akonadi_isolated_test(monitortest.cpp)
add_akonadi_isolated_test(monitorfiltertest.cpp)
add_akonadi_isolated_test(searchjobtest.cpp)
add_akonadi_isolated_test(changerecordertest.cpp)
add_akonadi_isolated_test(resourcetest.cpp)
add_akonadi_isolated_test(subscriptiontest.cpp)
add_akonadi_isolated_test(transactiontest.cpp)
add_akonadi_isolated_test(itemcopytest.cpp)
add_akonadi_isolated_test(itemmovetest.cpp)
add_akonadi_isolated_test(collectioncopytest.cpp)
add_akonadi_isolated_test(collectionmovetest.cpp)
add_akonadi_isolated_test(collectionsynctest.cpp)
add_akonadi_isolated_test(itemsynctest.cpp)
add_akonadi_isolated_test(linktest.cpp)
add_akonadi_isolated_test(cachetest.cpp)
add_akonadi_isolated_test(servermanagertest.cpp)
add_akonadi_isolated_test(collectioncreator.cpp)
add_akonadi_isolated_test(itembenchmark.cpp)
