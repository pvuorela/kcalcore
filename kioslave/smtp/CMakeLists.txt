

add_subdirectory(tests)

add_definitions("-DQT_NO_CAST_FROM_ASCII -DQT_NO_CAST_TO_ASCII")

set(smtp_optional_includes)
set(smtp_optional_libs)

if (SASL2_FOUND)
   set(smtp_optional_includes ${smtp_optional_includes} ${SASL2_INCLUDE_DIR})
   set(smtp_optional_libs     ${smtp_optional_libs}     ${SASL2_LIBRARIES})
endif()

include_directories(  ${smtp_optional_includes}  )


########### next target ###############

set(kio_smtp_PART_SRCS
   smtp.cpp
   request.cpp
   response.cpp
   capabilities.cpp
   command.cpp
   transactionstate.cpp
   smtpsessioninterface.cpp
   kioslavesession.cpp
)


kde4_add_plugin(kio_smtp ${kio_smtp_PART_SRCS})


target_link_libraries(kio_smtp  ${KDE4_KIO_LIBS} ${smtp_optional_libs})

install(TARGETS kio_smtp  DESTINATION ${PLUGIN_INSTALL_DIR} )

########### install files ###############

install( FILES smtp.protocol smtps.protocol  DESTINATION  ${SERVICES_INSTALL_DIR} )

